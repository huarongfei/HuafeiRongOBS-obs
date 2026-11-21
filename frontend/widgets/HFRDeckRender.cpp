#include "HFRDeckRender.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <cstdio>
#include <cstring>

#include <wincodec.h>

namespace hfr {
namespace {

/* GDI 文本光栅化：白字黑底 → alpha = 亮度，颜色由 run 指定 */
struct GdiText {
	HDC dc = nullptr;
	HBITMAP bmp = nullptr;
	HGDIOBJ oldBmp = nullptr;
	uint8_t *bits = nullptr;
	int w = 0, h = 0;

	bool Create(int width, int height)
	{
		if (width <= 0 || height <= 0 || width > 8192 || height > 8192) {
			return false;
		}
		dc = CreateCompatibleDC(nullptr);
		if (!dc) {
			return false;
		}
		BITMAPINFO bi;
		memset(&bi, 0, sizeof(bi));
		bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bi.bmiHeader.biWidth = width;
		bi.bmiHeader.biHeight = -height; /* 自顶向下 */
		bi.bmiHeader.biPlanes = 1;
		bi.bmiHeader.biBitCount = 32;
		bi.bmiHeader.biCompression = BI_RGB;
		void *p = nullptr;
		bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
		if (!bmp || !p) {
			Destroy();
			return false;
		}
		bits = (uint8_t *)p;
		oldBmp = SelectObject(dc, bmp);
		memset(bits, 0, (size_t)width * height * 4);
		SetBkMode(dc, TRANSPARENT);
		SetTextAlign(dc, TA_LEFT | TA_TOP);
		w = width;
		h = height;
		return true;
	}
	void Destroy()
	{
		if (dc) {
			if (oldBmp) {
				SelectObject(dc, oldBmp);
			}
			if (bmp) {
				DeleteObject(bmp);
			}
			DeleteDC(dc);
		}
		dc = nullptr;
		bmp = nullptr;
		oldBmp = nullptr;
		bits = nullptr;
	}
	~GdiText() { Destroy(); }
};

/* ---------------- WIC 图片解码（CPU 预览与纹理共用） ---------------- */
bool DecodeImageToBgra(const std::vector<uint8_t> &data, std::vector<uint8_t> &out, int &w, int &h)
{
	if (data.empty()) {
		return false;
	}
	static bool comInited = false;
	if (!comInited) {
		CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
		comInited = true;
	}
	IWICImagingFactory *factory = nullptr;
	HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
	if (FAILED(hr) || !factory) {
		return false;
	}
	IWICStream *stream = nullptr;
	IWICBitmapDecoder *decoder = nullptr;
	IWICBitmapFrameDecode *frame = nullptr;
	IWICFormatConverter *conv = nullptr;
	bool ok = false;
	do {
		if (FAILED(factory->CreateStream(&stream)) || !stream) {
			break;
		}
		if (FAILED(stream->InitializeFromMemory(const_cast<BYTE *>(data.data()), (DWORD)data.size()))) {
			break;
		}
		if (FAILED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
		    !decoder) {
			break;
		}
		if (FAILED(decoder->GetFrame(0, &frame)) || !frame) {
			break;
		}
		UINT ww = 0, hh = 0;
		if (FAILED(frame->GetSize(&ww, &hh)) || ww == 0 || hh == 0) {
			break;
		}
		if (FAILED(factory->CreateFormatConverter(&conv)) || !conv) {
			break;
		}
		if (FAILED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
					    WICBitmapPaletteTypeCustom))) {
			break;
		}
		const UINT stride = ww * 4;
		out.assign((size_t)stride * hh, 0);
		if (FAILED(conv->CopyPixels(nullptr, stride, (UINT)out.size(), out.data()))) {
			break;
		}
		w = (int)ww;
		h = (int)hh;
		ok = true;
	} while (false);
	if (conv) {
		conv->Release();
	}
	if (frame) {
		frame->Release();
	}
	if (decoder) {
		decoder->Release();
	}
	if (stream) {
		stream->Release();
	}
	factory->Release();
	return ok;
}

/* 按比例裁剪 BGRA（a:srcRect）：就地改写像素缓冲并更新尺寸。
 * 负值表示"图片外扩"，v1 只做裁切，钳制回原图范围（不会越界/崩溃）。 */
void ApplyCrop(std::vector<uint8_t> &pix, int &w, int &h, double l, double t, double r, double b)
{
	if (pix.empty() || w <= 0 || h <= 0) {
		return;
	}
	if ((size_t)pix.size() < (size_t)w * (size_t)h * 4) {
		return; /* 缓冲不完整：放弃裁剪，保留原图 */
	}
	int x0 = (int)std::lround((double)w * l);
	int y0 = (int)std::lround((double)h * t);
	int x1 = w - (int)std::lround((double)w * r);
	int y1 = h - (int)std::lround((double)h * b);
	x0 = std::max(0, std::min(w - 1, x0));
	y0 = std::max(0, std::min(h - 1, y0));
	x1 = std::max(x0 + 1, std::min(w, x1));
	y1 = std::max(y0 + 1, std::min(h, y1));
	const int nw = x1 - x0;
	const int nh = y1 - y0;
	if (nw == w && nh == h) {
		return; /* 无需裁剪 */
	}
	std::vector<uint8_t> out((size_t)nw * (size_t)nh * 4, 0);
	for (int y = 0; y < nh; y++) {
		memcpy(&out[(size_t)y * (size_t)nw * 4], &pix[((size_t)(y0 + y) * (size_t)w + (size_t)x0) * 4],
		       (size_t)nw * 4);
	}
	pix = std::move(out);
	w = nw;
	h = nh;
}

HFONT MakeFont(const std::wstring &face, int px, bool bold, bool italic)
{
	LOGFONTW lf;
	memset(&lf, 0, sizeof(lf));
	lf.lfHeight = -px;
	lf.lfWeight = bold ? FW_BOLD : FW_NORMAL;
	lf.lfItalic = italic ? TRUE : FALSE;
	lf.lfCharSet = DEFAULT_CHARSET;
	lf.lfQuality = ANTIALIASED_QUALITY;
	lf.lfOutPrecision = OUT_TT_PRECIS;
	wcsncpy_s(lf.lfFaceName, LF_FACESIZE, face.empty() ? L"Microsoft YaHei" : face.c_str(), _TRUNCATE);
	return CreateFontIndirectW(&lf);
}

int MeasureRun(HDC dc, const std::wstring &s)
{
	if (s.empty()) {
		return 0;
	}
	SIZE sz;
	GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
	return sz.cx;
}

/* 贪心折行：CJK 可任意断行，拉丁优先在空格断 */
std::vector<std::wstring> WrapText(HDC dc, const std::wstring &text, int maxW)
{
	std::vector<std::wstring> lines;
	if (text.empty()) {
		lines.push_back(L"");
		return lines;
	}
	std::wstring cur;
	int lastSpace = -1;
	for (wchar_t ch : text) {
		if (ch == L'\n') {
			lines.push_back(cur);
			cur.clear();
			lastSpace = -1;
			continue;
		}
		std::wstring next = cur;
		next.push_back(ch);
		if (MeasureRun(dc, next) > maxW && !cur.empty()) {
			if (ch == L' ' || lastSpace < 0) {
				lines.push_back(cur);
				cur.clear();
				cur.push_back(ch);
				lastSpace = -1;
			} else {
				const std::wstring head = cur.substr(0, (size_t)lastSpace);
				const std::wstring tail = cur.substr((size_t)lastSpace + 1);
				lines.push_back(head);
				cur = tail;
				cur.push_back(ch);
				lastSpace = -1;
			}
		} else {
			cur = next;
			if (ch == L' ') {
				lastSpace = (int)cur.size() - 1;
			}
		}
	}
	if (!cur.empty() || lines.empty()) {
		lines.push_back(cur);
	}
	return lines;
}

/* 把一个形状的文本光栅化为 BGRA（含 alpha），返回是否成功 */
bool RasterizeText(const Shape &sh, const Deck &deck, double scale, double ptScale, std::vector<uint8_t> &out, int &outW,
		   int &outH, const std::map<std::wstring, std::wstring> *fontSubst)
{
	const int boxW = std::max(1, (int)std::lround(sh.w * scale));
	const int boxH = std::max(1, (int)std::lround(sh.h * scale));
	GdiText g;
	if (!g.Create(boxW, boxH)) {
		return false;
	}
	/* 文本先按白色绘制到黑底掩膜，随后按 run 颜色着色 */
	SetTextColor(g.dc, RGB(255, 255, 255));
	/* bodyPr 内边距（EMU→px） */
	const int inL = (int)std::lround(sh.lIns * scale);
	const int inT = (int)std::lround(sh.tIns * scale);
	const int inR = (int)std::lround(sh.rIns * scale);
	const int inB = (int)std::lround(sh.bIns * scale);
	int penY = inT;
	for (const Para &para : sh.paras) {
		if (para.spaceBeforePt > 0.0) {
			penY += (int)std::lround(para.spaceBeforePt * ptScale);
		}
		/* 缩进：优先取文档 marL（EMU→px），否则按级别估算 */
		const int indent = (para.marL > 0) ? (int)std::lround(para.marL * scale)
						   : para.level * (int)std::lround(24 * ptScale);
		int maxFontPx = (int)std::lround(18 * ptScale * sh.fontScale);
		for (const Run &r : para.runs) {
			maxFontPx = std::max(maxFontPx,
					     (int)std::lround(r.sizePt * ptScale * sh.fontScale));
		}
		const int lineH = (int)std::lround(maxFontPx * 1.22 * (para.lineSpacing > 0.1 ? para.lineSpacing : 1.0));
		const int availW = std::max(16, boxW - inL - inR - indent);
		struct Seg {
			std::wstring text;
			const Run *run = nullptr;
			int fontPx = 12;
		};
		std::vector<std::vector<Seg>> lines;
		lines.emplace_back();
		int lineW = 0;
		bool firstRun = true;
		for (const Run &r : para.runs) {
			if (r.text.empty()) {
				continue;
			}
			std::wstring face = !r.ea.empty() ? r.ea : (!r.latin.empty() ? r.latin : deck.minorFont);
			if (fontSubst) {
				auto sit = fontSubst->find(face);
				if (sit != fontSubst->end() && !sit->second.empty()) {
					face = sit->second;
				}
			}
			const int fontPx = std::max(
				4, (int)std::lround((r.sizePt > 0 ? r.sizePt : 18.0) * ptScale * sh.fontScale));
			HFONT font = MakeFont(face, fontPx, r.bold, r.italic);
			HGDIOBJ oldFont = SelectObject(g.dc, font);
			std::wstring body = r.text;
			if (firstRun) {
				if (!para.bulletChar.empty()) {
					body = para.bulletChar + L" " + body;
				} else if (para.bullet) {
					body = std::wstring(L"• ") + body;
				}
				firstRun = false;
			}
			const int spcPx = (int)std::lround(r.spcPt * ptScale);
			for (wchar_t ch : body) {
				if (ch == L'\n') {
					lines.emplace_back();
					lineW = 0;
					continue;
				}
				SIZE sz = {};
				GetTextExtentPoint32W(g.dc, &ch, 1, &sz);
				const int cw = sz.cx + spcPx;
				if (lineW + cw > availW && lineW > 0) {
					lines.emplace_back();
					lineW = 0;
				}
				if (lines.back().empty() || lines.back().back().run != &r ||
				    lines.back().back().fontPx != fontPx) {
					Seg sg;
					sg.run = &r;
					sg.fontPx = fontPx;
					lines.back().push_back(std::move(sg));
				}
				lines.back().back().text.push_back(ch);
				lineW += cw;
			}
			SelectObject(g.dc, oldFont);
			DeleteObject(font);
		}
		/* 绘制：同一行内多 run 混排（下划线/删除线/上下标/字距按 run 生效） */
		for (const std::vector<Seg> &line : lines) {
			int x = inL + indent;
			int totalW = 0;
			for (const Seg &sg : line) {
				std::wstring face = !sg.run->ea.empty() ? sg.run->ea
							: (!sg.run->latin.empty() ? sg.run->latin : deck.minorFont);
				if (fontSubst) {
					auto sit = fontSubst->find(face);
					if (sit != fontSubst->end() && !sit->second.empty()) {
						face = sit->second;
					}
				}
				HFONT font = MakeFont(face, sg.fontPx, sg.run->bold, sg.run->italic);
				HGDIOBJ oldFont = SelectObject(g.dc, font);
				const int spcPx = (int)std::lround(sg.run->spcPt * ptScale);
				SetTextCharacterExtra(g.dc, spcPx);
				totalW += MeasureRun(g.dc, sg.text) + spcPx * (int)sg.text.size();
				SelectObject(g.dc, oldFont);
				DeleteObject(font);
			}
			if (para.align == 1) {
				x = inL + indent + std::max(0, (availW - totalW) / 2);
			} else if (para.align == 2) {
				x = inL + indent + std::max(0, availW - totalW);
			}
			for (const Seg &sg : line) {
				if (sg.text.empty()) {
					continue;
				}
				std::wstring face = !sg.run->ea.empty() ? sg.run->ea
							: (!sg.run->latin.empty() ? sg.run->latin : deck.minorFont);
				if (fontSubst) {
					auto sit = fontSubst->find(face);
					if (sit != fontSubst->end() && !sit->second.empty()) {
						face = sit->second;
					}
				}
				HFONT font = MakeFont(face, sg.fontPx, sg.run->bold, sg.run->italic);
				HGDIOBJ oldFont = SelectObject(g.dc, font);
				const int spcPx = (int)std::lround(sg.run->spcPt * ptScale);
				SetTextCharacterExtra(g.dc, spcPx);
				const int segW = MeasureRun(g.dc, sg.text) + spcPx * (int)sg.text.size();
				const int baseDy = (sg.run->baseline != 0)
							   ? -(int)std::lround((double)sg.run->baseline / 100000.0 * (double)sg.fontPx)
							   : 0;
				if (penY + baseDy < boxH) {
					TextOutW(g.dc, x, penY + baseDy, sg.text.c_str(), (int)sg.text.size());
					HBRUSH br = CreateSolidBrush(RGB(255, 255, 255));
					if (sg.run->underline) {
						RECT ur = {x, penY + baseDy + (int)(sg.fontPx * 0.92), x + segW,
							   penY + baseDy + (int)(sg.fontPx * 0.92) + std::max(1, sg.fontPx / 14)};
						FillRect(g.dc, &ur, br);
					}
					if (sg.run->strike) {
						RECT sr = {x, penY + baseDy + (int)(sg.fontPx * 0.55), x + segW,
							   penY + baseDy + (int)(sg.fontPx * 0.55) + std::max(1, sg.fontPx / 14)};
						FillRect(g.dc, &sr, br);
					}
					DeleteObject(br);
				}
				x += segW;
				SetTextCharacterExtra(g.dc, 0);
				SelectObject(g.dc, oldFont);
				DeleteObject(font);
			}
			penY += lineH;
		}
		if (para.runs.empty()) {
			penY += lineH;
		}
		if (para.spaceAfterPt > 0.0) {
			penY += (int)std::lround(para.spaceAfterPt * ptScale);
		}
	}
	GdiFlush();

	/* 白字黑底 → 用 run 颜色着色（此处整块统一取首个 run 的颜色，M0 简化） */
	uint32_t color = 0xFF000000u;
	bool got = false;
	for (const Para &p : sh.paras) {
		for (const Run &r : p.runs) {
			if (r.color.valid) {
				color = r.color.argb;
				got = true;
				break;
			}
		}
		if (got) {
			break;
		}
	}
	out.assign((size_t)boxW * boxH * 4, 0);
	const uint8_t cr = (uint8_t)((color >> 16) & 0xFF), cg = (uint8_t)((color >> 8) & 0xFF), cb = (uint8_t)(color & 0xFF);
	bool any = false;
	for (size_t i = 0; i < (size_t)boxW * boxH; i++) {
		const uint8_t *s = g.bits + i * 4;
		const uint8_t lum = std::max(s[0], std::max(s[1], s[2]));
		if (lum) {
			any = true;
		}
		out[i * 4 + 0] = cb;
		out[i * 4 + 1] = cg;
		out[i * 4 + 2] = cr;
		out[i * 4 + 3] = lum;
	}
	outW = boxW;
	outH = boxH;
	return any;
}

/* 图片镜像（flipH/flipV）：在解码/裁剪后的 BGRA 上翻转，CPU 预览与 GPU 纹理一致 */
static void MirrorBgraH(std::vector<uint8_t> &pix, int w, int h)
{
	if (w <= 1 || h <= 0) {
		return;
	}
	for (int y = 0; y < h; y++) {
		uint8_t *row = pix.data() + (size_t)y * (size_t)w * 4;
		for (int x = 0; x < w / 2; x++) {
			uint8_t *a = row + (size_t)x * 4;
			uint8_t *b = row + (size_t)(w - 1 - x) * 4;
			for (int k = 0; k < 4; k++) {
				std::swap(a[k], b[k]);
			}
		}
	}
}

static void MirrorBgraV(std::vector<uint8_t> &pix, int w, int h)
{
	if (h <= 1 || w <= 0) {
		return;
	}
	const size_t stride = (size_t)w * 4;
	for (int y = 0; y < h / 2; y++) {
		uint8_t *a = pix.data() + (size_t)y * stride;
		uint8_t *b = pix.data() + (size_t)(h - 1 - y) * stride;
		for (size_t k = 0; k < stride; k++) {
			std::swap(a[k], b[k]);
		}
	}
}

/* ---------------- SmartArt 简化渲染：树形布局 + 节点框 + 连线 ---------------- */
static void AddSmartArtShape(const Shape &sh, const Deck &deck, double scale, double ptScale, int x, int y,
			     int w, int h, int shapeIndex, RenderSlide &out,
			     const std::map<std::wstring, std::wstring> *fontSubst)
{
	const int n = (int)sh.smartNodes.size();
	if (n <= 0 || w <= 0 || h <= 0) {
		return;
	}
	std::vector<std::vector<int>> children((size_t)n);
	std::vector<int> roots;
	for (int i = 0; i < n; i++) {
		const int p = sh.smartNodes[(size_t)i].parent;
		if (p >= 0 && p < n && p != i) {
			children[(size_t)p].push_back(i);
		} else {
			roots.push_back(i);
		}
	}
	if (roots.empty()) {
		for (int i = 0; i < n; i++) {
			roots.push_back(i);
		}
	}
	/* 深度（BFS，防环：最多 n 层） */
	std::vector<int> depth((size_t)n, 0);
	std::vector<int> queue = roots;
	std::vector<bool> seen((size_t)n, false);
	for (int r : roots) {
		seen[(size_t)r] = true;
	}
	for (size_t qi = 0; qi < queue.size(); qi++) {
		const int cur = queue[qi];
		for (int c : children[(size_t)cur]) {
			if (!seen[(size_t)c]) {
				seen[(size_t)c] = true;
				depth[(size_t)c] = std::min(depth[(size_t)cur] + 1, n);
				queue.push_back(c);
			}
		}
	}
	/* 叶子槽位 + 内部节点取子节点均值 */
	std::vector<double> slotX((size_t)n, 0.0);
	double nextSlot = 0.0;
	std::vector<bool> laidOut((size_t)n, false);
	std::function<void(int)> layout = [&](int idx) {
		if (laidOut[(size_t)idx]) {
			return;
		}
		laidOut[(size_t)idx] = true;
		if (children[(size_t)idx].empty()) {
			slotX[(size_t)idx] = nextSlot;
			nextSlot += 1.0;
			return;
		}
		double sum = 0.0;
		int cnt = 0;
		for (int c : children[(size_t)idx]) {
			layout(c);
			sum += slotX[(size_t)c];
			cnt++;
		}
		slotX[(size_t)idx] = (cnt > 0) ? (sum / (double)cnt) : nextSlot++;
	};
	for (int r : roots) {
		layout(r);
	}
	for (int i = 0; i < n; i++) {
		if (!laidOut[(size_t)i]) {
			layout(i);
		}
	}
	/* 布局尺寸（单元坐标）→ 缩放到形状区域 */
	double minX = 1e9, maxX = -1e9, maxDepth = 0.0;
	for (int i = 0; i < n; i++) {
		minX = std::min(minX, slotX[(size_t)i]);
		maxX = std::max(maxX, slotX[(size_t)i]);
		maxDepth = std::max(maxDepth, (double)depth[(size_t)i]);
	}
	if (maxX < minX) {
		minX = maxX = 0.0;
	}
	const double nodeW = 1.0, nodeH = 0.55, hGap = 0.25, vGap = 0.95;
	const double extW = (maxX - minX + 1.0) * (nodeW + hGap);
	const double extH = (maxDepth + 1.0) * (nodeH + vGap) - vGap;
	const double sc = std::min((double)w / std::max(1e-6, extW), (double)h / std::max(1e-6, extH));
	const double padX = ((double)w - extW * sc) * 0.5;
	const double padY = ((double)h - extH * sc) * 0.5;
	auto nodePx = [&](int idx, double &px, double &py, double &nw, double &nh) {
		px = (double)x + padX + (slotX[(size_t)idx] - minX) * (nodeW + hGap) * sc;
		py = (double)y + padY + (double)depth[(size_t)idx] * (nodeH + vGap) * sc;
		nw = nodeW * sc;
		nh = nodeH * sc;
	};
	static const uint32_t palette[] = {0xFF4472C4u, 0xFFED7D31u, 0xFFA5A5A5u, 0xFFFFC000u, 0xFF5B9BD5u,
					   0xFF70AD47u};
	/* 连线（先画） */
	for (int i = 0; i < n; i++) {
		const int p = sh.smartNodes[(size_t)i].parent;
		if (p < 0 || p >= n) {
			continue;
		}
		double ppx, ppy, pnw, pnh, cpx, cpy, cnw, cnh;
		nodePx(p, ppx, ppy, pnw, pnh);
		nodePx(i, cpx, cpy, cnw, cnh);
		const int midY = (int)((ppy + pnh + cpy) * 0.5);
		const int lineW = std::max(1, (int)(2.0 * scale));
		auto addLine = [&](int lx, int ly, int lw, int lh) {
			Item ln;
			ln.kind = ItemKind::Rect;
			ln.x = lx;
			ln.y = ly;
			ln.w = std::max(1, lw);
			ln.h = std::max(1, lh);
			ln.color = 0xFF8C8C8Cu;
			ln.shapeIndex = shapeIndex;
			out.items.push_back(std::move(ln));
		};
		const int pcx = (int)(ppx + pnw * 0.5);
		const int ccx = (int)(cpx + cnw * 0.5);
		addLine(pcx, (int)(ppy + pnh), lineW, midY - (int)(ppy + pnh));
		addLine(std::min(pcx, ccx), midY, std::abs(ccx - pcx) + lineW, lineW);
		addLine(ccx, midY, lineW, (int)cpy - midY);
	}
	/* 节点 */
	for (int i = 0; i < n; i++) {
		double px, py, nw, nh;
		nodePx(i, px, py, nw, nh);
		const int nx = (int)px, ny = (int)py;
		const int nwi = std::max(4, (int)nw), nhi = std::max(4, (int)nh);
		Item bg;
		bg.kind = ItemKind::Rect;
		bg.x = nx;
		bg.y = ny;
		bg.w = nwi;
		bg.h = nhi;
		bg.color = palette[(size_t)(depth[(size_t)i] % 6)];
		bg.shapeIndex = shapeIndex;
		out.items.push_back(std::move(bg));
		/* 文本：用 RasterizeText 居中排版 */
		Shape label;
		label.kind = Shape::Kind::TextBox;
		label.w = (int64_t)((double)nwi / std::max(1e-6, scale));
		label.h = (int64_t)((double)nhi / std::max(1e-6, scale));
		label.paras.resize(1);
		label.paras[0].align = 1;
		Run run;
		run.text = sh.smartNodes[(size_t)i].text;
		const double targetPx = (double)nhi * 0.42;
		run.sizePt = std::max(6.0, std::min(28.0, targetPx / std::max(0.01, ptScale)));
		run.hasSize = true;
		run.color.valid = true;
		run.color.argb = 0xFFFFFFFFu;
		label.paras[0].runs.push_back(run);
		std::vector<uint8_t> pix;
		int tw = 0, th = 0;
		if (RasterizeText(label, deck, scale, ptScale, pix, tw, th, fontSubst)) {
			Item it;
			it.kind = ItemKind::Text;
			it.x = nx + (nwi - tw) / 2;
			it.y = ny + (nhi - th) / 2;
			it.w = tw;
			it.h = th;
			it.texW = tw;
			it.texH = th;
			it.shapeIndex = shapeIndex;
			it.pix = std::move(pix);
			out.items.push_back(std::move(it));
		}
	}
}
/* ---------------- 图表光栅化（GDI → BGRA；白底，供 OBS 纹理/离线预览复用） ---------------- */
namespace {
COLORREF ChartPalette(size_t i)
{
	static const COLORREF pal[] = {RGB(0x4F, 0x81, 0xBD), RGB(0xC0, 0x50, 0x4D), RGB(0x9B, 0xBB, 0x59),
				       RGB(0x80, 0x64, 0xA2), RGB(0x4B, 0xAC, 0xC6), RGB(0xF7, 0x96, 0x46)};
	return pal[i % 6];
}
} // namespace

bool RenderChartToBgra(const Shape &sh, int w, int h, std::vector<uint8_t> &out)
{
	if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || sh.chartSeries.empty()) {
		return false;
	}
	BITMAPINFO bi = {};
	bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = -h;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	void *bits = nullptr;
	HDC screen = GetDC(nullptr);
	HDC dc = CreateCompatibleDC(screen);
	HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
	if (!dc || !bmp || !bits) {
		if (bmp) DeleteObject(bmp);
		if (dc) DeleteDC(dc);
		if (screen) ReleaseDC(nullptr, screen);
		return false;
	}
	HGDIOBJ oldBmp = SelectObject(dc, bmp);
	RECT full = {0, 0, w, h};
	HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
	FillRect(dc, &full, white);
	DeleteObject(white);
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, RGB(70, 70, 70));
	const int fontH = std::max(11, std::min(16, w / 42));
	HFONT font = CreateFontW(-fontH, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
				  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
				  DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
	HGDIOBJ oldFont = SelectObject(dc, font);

	const int left = std::max(30, w / 14), right = std::max(8, w / 40);
	const int top = sh.chartTitle.empty() ? std::max(8, h / 24) : std::max(20, h / 6);
	const int bottomCats = std::max(16, h / 7);
	const int bottomLegend = 18;
	const int plotL = left, plotR = w - right, plotT = top, plotB = h - bottomCats - bottomLegend;
	const int plotW = std::max(1, plotR - plotL), plotH = std::max(1, plotB - plotT);

	if (!sh.chartTitle.empty()) {
		RECT tr = {0, 4, w, top + 2};
		DrawTextW(dc, sh.chartTitle.c_str(), -1, &tr, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
	}

	const size_t nSeries = sh.chartSeries.size();
	if (sh.chartType == L"pie") {
		double total = 0.0;
		for (const auto &s : sh.chartSeries) {
			for (double v : s.vals) total += std::max(0.0, v);
		}
		if (total <= 0) total = 1.0;
		const int cx0 = plotL + plotW / 2, cy0 = plotT + plotH / 2;
		const int rad = std::max(8, std::min(plotW, plotH) / 2 - 4);
		double ang = -3.14159265358979 / 2;
		size_t ci = 0;
		const auto &s0 = sh.chartSeries[0];
		for (size_t i = 0; i < s0.vals.size(); i++) {
			double v = std::max(0.0, s0.vals[i]);
			if (v <= 0) continue;
			double sweep = v / total * 2.0 * 3.14159265358979;
			int x1 = cx0 + (int)(rad * std::cos(ang)), y1 = cy0 + (int)(rad * std::sin(ang));
			int x2 = cx0 + (int)(rad * std::cos(ang + sweep)), y2 = cy0 + (int)(rad * std::sin(ang + sweep));
			HBRUSH b = CreateSolidBrush(ChartPalette(ci++));
			HGDIOBJ ob = SelectObject(dc, b);
			Pie(dc, cx0 - rad, cy0 - rad, cx0 + rad, cy0 + rad, x1, y1, x2, y2);
			SelectObject(dc, ob);
			DeleteObject(b);
			ang += sweep;
		}
	} else if (sh.chartType == L"line") {
		double maxV = 1.0;
		for (const auto &s : sh.chartSeries) {
			for (double v : s.vals) maxV = std::max(maxV, v);
		}
		int nCat = (int)sh.chartCats.size();
		for (const auto &s : sh.chartSeries) nCat = std::max(nCat, (int)s.vals.size());
		nCat = std::max(nCat, 1);
		for (size_t si = 0; si < nSeries; si++) {
			HPEN pen = CreatePen(PS_SOLID, 2, ChartPalette(si));
			HGDIOBJ op = SelectObject(dc, pen);
			bool first = true;
			for (int i = 0; i < (int)sh.chartSeries[si].vals.size(); i++) {
				int x = plotL + (int)((double)i / (double)std::max(1, nCat - 1) * (double)plotW);
				int y = plotB - (int)(sh.chartSeries[si].vals[i] / maxV * (double)plotH);
				if (first) {
					MoveToEx(dc, x, y, nullptr);
					first = false;
				} else {
					LineTo(dc, x, y);
				}
			}
			SelectObject(dc, op);
			DeleteObject(pen);
		}
	} else {
		double maxV = 1.0;
		for (const auto &s : sh.chartSeries) {
			for (double v : s.vals) maxV = std::max(maxV, v);
		}
		int nCat = (int)sh.chartCats.size();
		for (const auto &s : sh.chartSeries) nCat = std::max(nCat, (int)s.vals.size());
		nCat = std::max(nCat, 1);
		HPEN gp = CreatePen(PS_SOLID, 1, RGB(220, 220, 220));
		HGDIOBJ op = SelectObject(dc, gp);
		for (int g = 0; g <= 4; g++) {
			int y = plotB - (int)((double)g / 4.0 * (double)plotH);
			MoveToEx(dc, plotL, y, nullptr);
			LineTo(dc, plotR, y);
		}
		SelectObject(dc, op);
		DeleteObject(gp);
		const int groupW = std::max(1, plotW / nCat);
		const int barW = std::max(2, (int)((double)groupW * 0.8 / (double)std::max<size_t>(1, nSeries)));
		for (int i = 0; i < nCat; i++) {
			for (size_t si = 0; si < nSeries; si++) {
				if (i >= (int)sh.chartSeries[si].vals.size()) continue;
				double v = std::max(0.0, sh.chartSeries[si].vals[i]);
				int bh = (int)(v / maxV * (double)plotH);
				int x0 = plotL + i * groupW + (groupW - barW * (int)nSeries) / 2 + (int)si * barW;
				int y0 = plotB - bh;
				HBRUSH b = CreateSolidBrush(ChartPalette(si));
				RECT br = {x0, y0, x0 + barW - 1, plotB};
				FillRect(dc, &br, b);
				DeleteObject(b);
			}
		}
	}

	/* 坐标轴 */
	{
		HPEN ap = CreatePen(PS_SOLID, 1, RGB(160, 160, 160));
		HGDIOBJ op = SelectObject(dc, ap);
		MoveToEx(dc, plotL, plotT, nullptr);
		LineTo(dc, plotL, plotB);
		MoveToEx(dc, plotL, plotB, nullptr);
		LineTo(dc, plotR, plotB);
		SelectObject(dc, op);
		DeleteObject(ap);
	}

	/* 类别标签 */
	{
		int nCat = (int)sh.chartCats.size();
		for (const auto &s : sh.chartSeries) nCat = std::max(nCat, (int)s.vals.size());
		if (nCat > 0) {
			const int step = std::max(1, nCat / 12);
			const int groupW = std::max(1, plotW / nCat);
			for (int i = 0; i < nCat; i += step) {
				std::wstring label = (i < (int)sh.chartCats.size()) ? sh.chartCats[(size_t)i]
									   : L"#" + std::to_wstring(i + 1);
				RECT r = {plotL + i * groupW, plotB + 2, plotL + (i + 1) * groupW, plotB + bottomCats};
				DrawTextW(dc, label.c_str(), -1, &r, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
			}
		}
	}

	/* 图例（柱/折线） */
	if (nSeries > 0 && sh.chartType != L"pie") {
		int lx = plotL;
		const int ly = h - bottomLegend + 3;
		for (size_t si = 0; si < nSeries && lx < plotR - 20; si++) {
			HBRUSH b = CreateSolidBrush(ChartPalette(si));
			RECT sr = {lx, ly, lx + 10, ly + 10};
			FillRect(dc, &sr, b);
			DeleteObject(b);
			lx += 14;
			std::wstring name = sh.chartSeries[si].name.empty()
						    ? L"系列" + std::to_wstring(si + 1)
						    : sh.chartSeries[si].name;
			SIZE sz = {};
			GetTextExtentPoint32W(dc, name.c_str(), (int)name.size(), &sz);
			TextOutW(dc, lx, ly - 1, name.c_str(), (int)name.size());
			lx += sz.cx + 18;
		}
	}

	out.resize((size_t)w * (size_t)h * 4);
	std::memcpy(out.data(), bits, out.size());
	SelectObject(dc, oldFont);
	DeleteObject(font);
	SelectObject(dc, oldBmp);
	DeleteObject(bmp);
	DeleteDC(dc);
	ReleaseDC(nullptr, screen);
	return true;
}
/* 简化外阴影：偏移半透明矩形（单位 EMU → px） */
static void AddShadowRect(const Shape &sh, double scale, int x, int y, int w, int h, int shapeIndex,
			  RenderSlide &out)
{
	if (!sh.hasShadow) {
		return;
	}
	const double rad = sh.shadowDirDeg * 3.14159265358979 / 180.0;
	const int ox = (int)std::lround(std::cos(rad) * sh.shadowDist * scale);
	const int oy = (int)std::lround(std::sin(rad) * sh.shadowDist * scale);
	uint32_t alpha = 128;
	if (sh.shadowColor.valid) {
		alpha = (sh.shadowColor.argb >> 24) & 0xFF;
	}
	Item sd;
	sd.kind = ItemKind::Rect;
	sd.x = x + ox;
	sd.y = y + oy;
	sd.w = w;
	sd.h = h;
	sd.color = (alpha << 24) | 0x000000u;
	if (sh.shadowColor.valid) {
		sd.color = (alpha << 24) | (sh.shadowColor.argb & 0x00FFFFFFu);
	}
	sd.shapeIndex = shapeIndex;
	out.items.push_back(std::move(sd));
}

/* 形状/图片描边：四边矩形（不支持圆角/虚线，v1 简化） */
static void AddOutline(const Shape &sh, double scale, int x, int y, int w, int h, int shapeIndex,
		       RenderSlide &out)
{
	if (!sh.hasLine || !sh.line.valid) {
		return;
	}
	const int t = std::max(1, (int)std::lround((double)sh.lineW * scale));
	auto add = [&](int rx, int ry, int rw, int rh) {
		Item ln;
		ln.kind = ItemKind::Rect;
		ln.x = rx;
		ln.y = ry;
		ln.w = std::max(1, rw);
		ln.h = std::max(1, rh);
		ln.color = sh.line.argb;
		ln.shapeIndex = shapeIndex;
		out.items.push_back(std::move(ln));
	};
	add(x, y, w, t);
	add(x, y + h - t, w, t);
	add(x, y, t, h);
	add(x + w - t, y, t, h);
}

void AddShape(const Shape &sh, const Deck &deck, double scale, double ptScale, int dx, int dy, int shapeIndex,
	       RenderSlide &out, const std::map<std::wstring, std::wstring> *fontSubst)
{
	const int x = dx + (int)std::lround(sh.x * scale);
	const int y = dy + (int)std::lround(sh.y * scale);
	const int w = std::max(1, (int)std::lround(sh.w * scale));
	const int h = std::max(1, (int)std::lround(sh.h * scale));

	if (sh.kind == Shape::Kind::Group) {
		for (const Shape &c : sh.children) {
			AddShape(c, deck, scale, ptScale, x, y, shapeIndex, out, fontSubst);
		}
		return;
	}
	if (sh.kind == Shape::Kind::Picture) {
		Item it;
		it.kind = ItemKind::Picture;
		it.x = x;
		it.y = y;
		it.w = w;
		it.h = h;
		it.rot60k = (int)sh.rot;
		it.debug = sh.name;
		it.shapeIndex = shapeIndex;
		/* 优先解码为 BGRA（CPU 预览与 GPU 纹理都直接用） */
		std::vector<uint8_t> decoded;
		int dw = 0, dh = 0;
		if (DecodeImageToBgra(sh.imageData, decoded, dw, dh)) {
			/* 裁剪在解码后完成：CPU 预览与 OBS 纹理都直接用裁剪后的 pix */
			if (sh.hasCrop) {
				ApplyCrop(decoded, dw, dh, sh.cropL, sh.cropT, sh.cropR, sh.cropB);
			}
			if (sh.flipH) {
				MirrorBgraH(decoded, dw, dh);
			}
			if (sh.flipV) {
				MirrorBgraV(decoded, dw, dh);
			}
			/* 图片亮度/对比度（a:blip/a:lum）：out = (in-128)*(1+c)+128 + b*255 */
			if (sh.bright != 0.0 || sh.contrast != 0.0) {
				const double c = sh.contrast;
				const double b = sh.bright * 255.0;
				for (size_t i = 0; i + 3 < decoded.size(); i += 4) {
					for (int k = 0; k < 3; k++) {
						double v = (double)decoded[i + k];
						v = (v - 128.0) * (1.0 + c) + 128.0 + b;
						decoded[i + k] = (uint8_t)std::max(0.0, std::min(255.0, v));
					}
				}
			}
			it.pix = std::move(decoded);
			it.texW = dw;
			it.texH = dh;
		} else {
			it.pix = sh.imageData; /* 退回：保留原始编码字节 */
			it.ext = sh.imageExt;
		}
		out.items.push_back(std::move(it));
		AddOutline(sh, scale, x, y, w, h, shapeIndex, out);
		return;
	}
	/* 暂不支持的复杂对象（SmartArt/媒体/OLE）：占位框 + 明确提示，避免空白 */
	if (!sh.unsupported.empty() && (sh.kind == Shape::Kind::Unknown || sh.kind == Shape::Kind::Media)) {
		Item bg;
		bg.kind = ItemKind::Rect;
		bg.x = x;
		bg.y = y;
		bg.w = w;
		bg.h = h;
		bg.color = 0xFF3B4A5A;
		bg.shapeIndex = shapeIndex;
		bg.debug = sh.name;
		out.items.push_back(std::move(bg));

		Shape label;
		label.kind = Shape::Kind::TextBox;
		label.w = sh.w;
		label.h = sh.h;
		label.paras.resize(1);
		label.paras[0].align = 1; /* 居中 */
		Run run;
		run.text = L"暂不支持：" + sh.unsupported;
		run.sizePt = 14.0;
		run.hasSize = true;
		run.color.valid = true;
		run.color.argb = 0xFFFFFFFFu;
		label.paras[0].runs.push_back(run);
		std::vector<uint8_t> pix;
		int tw = 0, th = 0;
		if (RasterizeText(label, deck, scale, ptScale, pix, tw, th, fontSubst)) {
			Item it;
			it.kind = ItemKind::Text;
			it.x = x + (w - tw) / 2;
			it.y = y + (h - th) / 2;
			it.w = tw;
			it.h = th;
			it.texW = tw;
			it.texH = th;
			it.shapeIndex = shapeIndex;
			it.pix = std::move(pix);
			out.items.push_back(std::move(it));
		}
		return;
	}
	if (sh.isSmartArt && !sh.smartNodes.empty()) {
		AddSmartArtShape(sh, deck, scale, ptScale, x, y, w, h, shapeIndex, out, fontSubst);
		return;
	}
	if (sh.kind == Shape::Kind::Chart) {
		std::vector<uint8_t> chartPix;
		if (RenderChartToBgra(sh, w, h, chartPix)) {
			Item it;
			it.kind = ItemKind::Picture; /* 已光栅化 BGRA，走图片通路（OBS/预览都直接可用） */
			it.x = x;
			it.y = y;
			it.w = w;
			it.h = h;
			it.rot60k = (int)sh.rot;
			it.debug = sh.name;
			it.shapeIndex = shapeIndex;
			it.pix = std::move(chartPix);
			it.texW = w;
			it.texH = h;
			out.items.push_back(std::move(it));
		}
		return;
	}
	/* 渐变填充：CPU 生成渐变位图（限制到合理分辨率） */
	if (sh.kind != Shape::Kind::Picture && sh.kind != Shape::Kind::Group &&
	    sh.kind != Shape::Kind::Table && sh.kind != Shape::Kind::Chart &&
	    (sh.hasFill || sh.hasGradient)) {
		AddShadowRect(sh, scale, x, y, w, h, shapeIndex, out);
	}
	if (sh.hasGradient && sh.gradStops.size() >= 2) {
		const int gw = std::max(2, std::min(w, 1024));
		const int gh = std::max(2, std::min(h, 1024));
		Item it;
		it.kind = ItemKind::Gradient;
		it.x = x;
		it.y = y;
		it.w = w;
		it.h = h;
		it.shapeIndex = shapeIndex;
		it.rot60k = (int)sh.rot;
		it.debug = sh.name;
		it.pix.assign((size_t)gw * gh * 4, 0);
		const double rad = sh.gradAngleDeg * 3.14159265358979 / 180.0;
		const double ux = cos(rad), uy = sin(rad);
		for (int py = 0; py < gh; py++) {
			for (int px = 0; px < gw; px++) {
				const double nx = (double)px / (double)(gw - 1);
				const double ny = (double)py / (double)(gh - 1);
				double t = nx * ux + ny * uy;
				t = std::max(0.0, std::min(1.0, t));
				/* 找到 t 所在区间并插值 */
				const GradientStop *a = &sh.gradStops[0];
				const GradientStop *b = &sh.gradStops[sh.gradStops.size() - 1];
				for (size_t i = 0; i + 1 < sh.gradStops.size(); i++) {
					if (t >= sh.gradStops[i].pos && t <= sh.gradStops[i + 1].pos) {
						a = &sh.gradStops[i];
						b = &sh.gradStops[i + 1];
						break;
					}
				}
				const double span = (b->pos - a->pos);
				const double f = (span > 1e-6) ? ((t - a->pos) / span) : 0.0;
				const uint8_t ar = (uint8_t)((a->color.argb >> 16) & 0xFF);
				const uint8_t ag = (uint8_t)((a->color.argb >> 8) & 0xFF);
				const uint8_t ab = (uint8_t)(a->color.argb & 0xFF);
				const uint8_t br = (uint8_t)((b->color.argb >> 16) & 0xFF);
				const uint8_t bg = (uint8_t)((b->color.argb >> 8) & 0xFF);
				const uint8_t bb = (uint8_t)(b->color.argb & 0xFF);
				const size_t idx = ((size_t)py * gw + (size_t)px) * 4;
				it.pix[idx + 0] = (uint8_t)(ab + (bb - ab) * f);
				it.pix[idx + 1] = (uint8_t)(ag + (bg - ag) * f);
				it.pix[idx + 2] = (uint8_t)(ar + (br - ar) * f);
				it.pix[idx + 3] = 255;
			}
		}
		it.texW = gw;
		it.texH = gh;
		out.items.push_back(std::move(it));
	} else if (sh.hasFill) {
		Item it;
		it.kind = ItemKind::Rect;
		it.x = x;
		it.y = y;
		it.w = w;
		it.h = h;
		it.color = sh.fill.argb;
		it.shapeIndex = shapeIndex;
		it.rot60k = (int)sh.rot;
		it.debug = sh.name;
		it.shapeIndex = shapeIndex;
		out.items.push_back(std::move(it));
	}
	if (sh.kind != Shape::Kind::Table && sh.kind != Shape::Kind::Chart &&
	    sh.kind != Shape::Kind::Picture) {
		AddOutline(sh, scale, x, y, w, h, shapeIndex, out);
	}
	if (sh.kind == Shape::Kind::Table) {
		if (sh.cells.empty() || sh.colW.empty()) {
			Item it;
			it.kind = ItemKind::Table;
			it.x = x;
			it.y = y;
			it.w = w;
			it.h = h;
			it.debug = sh.name;
			it.shapeIndex = shapeIndex;
			out.items.push_back(std::move(it));
			return;
		}
		/* 列宽/行高归一化到表格实际尺寸 */
		double totalCol = 0, totalRow = 0;
		for (double c : sh.colW) {
			totalCol += c;
		}
		for (double r : sh.rowH) {
			totalRow += r;
		}
		if (totalCol < 1.0) {
			totalCol = 1.0;
		}
		if (totalRow < 1.0) {
			totalRow = 1.0;
		}
		const int rowsN = (int)sh.cells.size();
		const int colsN = (int)sh.colW.size();
		std::vector<int> colPx((size_t)colsN), colPre((size_t)colsN + 1, 0);
		for (int i = 0; i < colsN; i++) {
			colPx[(size_t)i] = std::max(1, (int)std::lround(sh.colW[(size_t)i] / totalCol * (double)w));
			colPre[(size_t)i + 1] = colPre[(size_t)i] + colPx[(size_t)i];
		}
		std::vector<int> rowPx((size_t)std::max(1, rowsN), (int)((double)h / std::max(1, rowsN))), rowPre((size_t)rowsN + 1, 0);
		for (int i = 0; i < rowsN && i < (int)sh.rowH.size(); i++) {
			rowPx[(size_t)i] = std::max(1, (int)std::lround(sh.rowH[(size_t)i] / totalRow * (double)h));
		}
		for (int i = 0; i < rowsN; i++) {
			rowPre[(size_t)i + 1] = rowPre[(size_t)i] + rowPx[(size_t)i];
		}
		bool anyBorder = false, anyMerge = false;
		for (const auto &rowCells : sh.cells) {
			for (const TableCell &cc : rowCells) {
				anyBorder = anyBorder || cc.borderL || cc.borderR || cc.borderT || cc.borderB;
				anyMerge = anyMerge || cc.hMerge || cc.vMerge || cc.gridSpan > 1 || cc.rowSpan > 1;
			}
		}
		/* 表格样式：表头 / 隔行带纹（仅在单元格无显式填充时使用） */
		uint32_t headColor = 0, bandColor = 0;
		bool hasHead = false, hasBand = false;
		if (sh.tblFirstRow || sh.tblBandRow) {
			uint32_t accent = 0xFF4472C4u;
			auto itc = deck.themeColors.find(L"accent1");
			if (itc != deck.themeColors.end() && itc->second.size() == 6) {
				const unsigned v = (unsigned)wcstoul(itc->second.c_str(), nullptr, 16);
				accent = 0xFF000000u | ((v & 0xFF) << 16) | (v & 0xFF00) | ((v >> 16) & 0xFF);
			}
			auto mix = [](uint32_t c, int pct) {
				const int r = (int)((c >> 16) & 0xFF), g = (int)((c >> 8) & 0xFF), b = (int)(c & 0xFF);
				const int nr = r + (255 - r) * pct / 100;
				const int ng = g + (255 - g) * pct / 100;
				const int nb = b + (255 - b) * pct / 100;
				return 0xFF000000u | ((uint32_t)nr << 16) | ((uint32_t)ng << 8) | (uint32_t)nb;
			};
			headColor = mix(accent, 78);
			bandColor = mix(accent, 90);
			hasHead = sh.tblFirstRow;
			hasBand = sh.tblBandRow;
		}
		const uint32_t gridColor = 0xFF9E9E9Eu;
		for (int r = 0; r < rowsN; r++) {
			const int cy = y + rowPre[(size_t)r];
			int colCursor = 0;
			for (size_t ci = 0; ci < sh.cells[(size_t)r].size(); ci++) {
				const TableCell &cell = sh.cells[(size_t)r][ci];
				if (cell.hMerge || cell.vMerge) {
					colCursor += std::max(1, cell.gridSpan);
					continue;
				}
				if (colCursor >= colsN) {
					break;
				}
				const int spanC = std::max(1, std::min(cell.gridSpan, colsN - colCursor));
				const int spanR = std::max(1, std::min(cell.rowSpan, rowsN - r));
				const int cx = x + colPre[(size_t)colCursor];
				const int cw = colPre[(size_t)(colCursor + spanC)] - colPre[(size_t)colCursor];
				const int ch = rowPre[(size_t)(r + spanR)] - rowPre[(size_t)r];
				const int mL = (int)std::lround(cell.marL * scale);
				const int mR = (int)std::lround(cell.marR * scale);
				const int mT = (int)std::lround(cell.marT * scale);
				const int mB = (int)std::lround(cell.marB * scale);
				uint32_t fillColor = 0;
				bool fill = false;
				if (cell.hasFill) {
					fillColor = cell.fill.argb;
					fill = true;
				} else if (hasHead && r == 0) {
					fillColor = headColor;
					fill = true;
				} else if (hasBand && (r % 2) == 1) {
					fillColor = bandColor;
					fill = true;
				}
				if (fill) {
					Item bg;
					bg.kind = ItemKind::Rect;
					bg.x = cx;
					bg.y = cy;
					bg.w = cw;
					bg.h = ch;
					bg.color = fillColor;
					bg.shapeIndex = shapeIndex;
					out.items.push_back(std::move(bg));
				}
				if (!cell.paras.empty()) {
					Shape cellShape;
					cellShape.kind = Shape::Kind::TextBox;
					cellShape.w = (int64_t)((double)std::max(8, cw - mL - mR) / scale);
					cellShape.h = (int64_t)((double)std::max(8, ch - mT - mB) / scale);
					cellShape.paras = cell.paras;
					Item t;
					t.kind = ItemKind::Text;
					t.x = cx + mL;
					t.y = cy + mT;
					t.shapeIndex = shapeIndex;
					t.debug = sh.name;
					int tw = 0, th = 0;
					if (RasterizeText(cellShape, deck, scale, ptScale, t.pix, tw, th, fontSubst)) {
						t.texW = tw;
						t.texH = th;
						t.w = tw;
						t.h = th;
						if (cell.anchor == 2) {
							t.y = cy + ch - mB - th;
						} else if (cell.anchor == 1) {
							t.y = cy + (ch - th) / 2;
						}
						t.y = std::max(cy, std::min(t.y, cy + ch - th));
						out.items.push_back(std::move(t));
					}
				}
				if (anyBorder) {
					const int bw = std::max(1, (int)std::lround(cell.borderWEmu * scale));
					auto addB = [&](int bx, int by, int bwid, int bhei, const Color &col) {
						Item ln;
						ln.kind = ItemKind::Rect;
						ln.x = bx;
						ln.y = by;
						ln.w = std::max(1, bwid);
						ln.h = std::max(1, bhei);
						ln.color = col.valid ? col.argb : gridColor;
						ln.shapeIndex = shapeIndex;
						out.items.push_back(std::move(ln));
					};
					if (cell.borderT) {
						addB(cx, cy, cw, bw, cell.borderColor);
					}
					if (cell.borderB) {
						addB(cx, cy + ch - bw, cw, bw, cell.borderColor);
					}
					if (cell.borderL) {
						addB(cx, cy, bw, ch, cell.borderColor);
					}
					if (cell.borderR) {
						addB(cx + cw - bw, cy, bw, ch, cell.borderColor);
					}
				} else if (!anyMerge && colCursor > 0) {
					Item ln;
					ln.kind = ItemKind::Rect;
					ln.x = cx;
					ln.y = cy;
					ln.w = 1;
					ln.h = ch;
					ln.color = gridColor;
					ln.shapeIndex = shapeIndex;
					out.items.push_back(std::move(ln));
				}
				colCursor += spanC;
			}
			if (!anyBorder && !anyMerge && r > 0) {
				Item ln;
				ln.kind = ItemKind::Rect;
				ln.x = x;
				ln.y = cy;
				ln.w = w;
				ln.h = 1;
				ln.color = gridColor;
				ln.shapeIndex = shapeIndex;
				out.items.push_back(std::move(ln));
			}
		}
		return;
	}
	bool hasText = false;
	for (const Para &p : sh.paras) {
		if (!p.runs.empty()) {
			hasText = true;
			break;
		}
	}
	if (hasText) {
		Item it;
		it.kind = ItemKind::Text;
		it.x = x;
		it.y = y;
		it.w = w;
		it.h = h;
		it.rot60k = (int)sh.rot;
		it.debug = sh.name;
		it.shapeIndex = shapeIndex;
		int tw = 0, th = 0;
		if (RasterizeText(sh, deck, scale, ptScale, it.pix, tw, th, fontSubst)) {
			it.texW = tw;
			it.texH = th;
			it.w = tw;
			it.h = th;
			out.items.push_back(std::move(it));
		}
	}
}

} // namespace

bool BuildSlide(const Deck &deck, int slideIndex, int targetW, int targetH, RenderSlide &out, std::wstring &err,
		const std::map<std::wstring, std::wstring> *fontSubst)
{
	if (slideIndex < 0 || slideIndex >= (int)deck.slides.size()) {
		err = L"页码超出范围";
		return false;
	}
	if (deck.slideW <= 0 || deck.slideH <= 0) {
		err = L"页面尺寸无效";
		return false;
	}
	out.items.clear();
	out.width = targetW;
	out.height = targetH;
	const double scaleX = (double)targetW / (double)deck.slideW;
	const double scaleY = (double)targetH / (double)deck.slideH;
	const double scale = std::min(scaleX, scaleY);
	/* 字号缩放：幻灯片高度换算为"磅"，pt → 像素 */
	const double slidePtH = (double)deck.slideH / 12700.0;
	const double ptScale = (slidePtH > 1.0) ? ((double)targetH / slidePtH) : 1.0;
	const int contentW = (int)std::lround(deck.slideW * scale);
	const int contentH = (int)std::lround(deck.slideH * scale);
	const int dx = (targetW - contentW) / 2;
	const int dy = (targetH - contentH) / 2;

	const std::vector<Shape> &shapes = deck.slides[(size_t)slideIndex].shapes;
	for (size_t si = 0; si < shapes.size(); si++) {
		AddShape(shapes[si], deck, scale, ptScale, dx, dy, (int)si, out, fontSubst);
	}
	return true;
}

static void BlendPixel(std::vector<uint8_t> &dst, int W, int x, int y, uint32_t color, uint8_t a)
{
	if (x < 0 || y < 0 || x >= W) {
		return;
	}
	const size_t idx = ((size_t)y * W + (size_t)x) * 4;
	if (idx + 3 >= dst.size()) {
		return;
	}
	const uint8_t sr = (uint8_t)((color >> 16) & 0xFF), sg = (uint8_t)((color >> 8) & 0xFF), sb = (uint8_t)(color & 0xFF);
	const int ia = 255 - a;
	dst[idx + 0] = (uint8_t)((sb * a + dst[idx + 0] * ia) / 255);
	dst[idx + 1] = (uint8_t)((sg * a + dst[idx + 1] * ia) / 255);
	dst[idx + 2] = (uint8_t)((sr * a + dst[idx + 2] * ia) / 255);
	dst[idx + 3] = 255;
}

void CompositeToBgra(const RenderSlide &slide, std::vector<uint8_t> &bgra)
{
	const int W = slide.width, H = slide.height;
	bgra.assign((size_t)W * H * 4, 0xFF); /* 白底（BGRA=255,255,255,255） */
	for (const Item &it : slide.items) {
		if (it.kind == ItemKind::Rect) {
			const int alpha = (int)((it.color >> 24) & 0xFF); /* 支持半透明（阴影等） */
			for (int y = it.y; y < it.y + it.h; y++) {
				for (int x = it.x; x < it.x + it.w; x++) {
					BlendPixel(bgra, W, x, y, it.color, alpha);
				}
			}
		} else if (it.kind == ItemKind::Picture && it.texW > 0 && it.texH > 0) {
			/* 实绘（最近邻缩放） */
			for (int y = 0; y < it.h; y++) {
				const int sy = (int)((int64_t)y * it.texH / std::max(1, it.h));
				for (int x = 0; x < it.w; x++) {
					const int sx = (int)((int64_t)x * it.texW / std::max(1, it.w));
					const size_t si = ((size_t)sy * (size_t)it.texW + (size_t)sx) * 4;
					if (si + 3 >= it.pix.size()) {
						continue;
					}
					const uint32_t c = 0xFF000000u | ((uint32_t)it.pix[si + 2] << 16) |
							   ((uint32_t)it.pix[si + 1] << 8) | (uint32_t)it.pix[si + 0];
					BlendPixel(bgra, W, it.x + x, it.y + y, c, 255);
				}
			}
		} else if (it.kind == ItemKind::Picture || it.kind == ItemKind::Table) {
			/* 无法解码时的占位 */
			for (int y = it.y; y < it.y + it.h; y++) {
				for (int x = it.x; x < it.x + it.w; x++) {
					if ((x - it.x) % 8 == 0 || (y - it.y) % 8 == 0) {
						BlendPixel(bgra, W, x, y, 0xFFBFBFBF, 255);
					}
				}
			}
		} else if (it.kind == ItemKind::Gradient) {
			for (int y = 0; y < it.texH; y++) {
				for (int x = 0; x < it.texW; x++) {
					const size_t si = ((size_t)y * it.texW + (size_t)x) * 4;
					const uint32_t c = 0xFF000000u | ((uint32_t)it.pix[si + 2] << 16) |
							   ((uint32_t)it.pix[si + 1] << 8) | (uint32_t)it.pix[si + 0];
					/* 位图按形状尺寸拉伸到目标区域 */
					const double sx = (double)it.texW / (double)std::max(1, it.w);
					const double sy = (double)it.texH / (double)std::max(1, it.h);
					const int dx = it.x + (int)((double)x / sx);
					const int dy = it.y + (int)((double)y / sy);
					BlendPixel(bgra, W, dx, dy, c, 255);
				}
			}
		} else if (it.kind == ItemKind::Text) {
			for (int y = 0; y < it.texH; y++) {
				for (int x = 0; x < it.texW; x++) {
					const size_t si = ((size_t)y * it.texW + (size_t)x) * 4;
					const uint8_t a = it.pix[si + 3];
					if (!a) {
						continue;
					}
					const uint32_t c = 0xFF000000u | ((uint32_t)it.pix[si + 2] << 16) | ((uint32_t)it.pix[si + 1] << 8) |
							   (uint32_t)it.pix[si + 0];
					BlendPixel(bgra, W, it.x + x, it.y + y, c, a);
				}
			}
		}
	}
}

bool WriteBmp32(const std::wstring &path, int w, int h, const std::vector<uint8_t> &bgra)
{
	FILE *f = nullptr;
	if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) {
		return false;
	}
	const uint32_t imgSize = (uint32_t)w * 4u * (uint32_t)h;
	const uint32_t fileSize = 54 + imgSize;
	uint8_t hdr[54];
	memset(hdr, 0, sizeof(hdr));
	hdr[0] = 'B';
	hdr[1] = 'M';
	memcpy(hdr + 2, &fileSize, 4);
	uint32_t off = 54;
	memcpy(hdr + 10, &off, 4);
	uint32_t hdrSize = 40;
	memcpy(hdr + 14, &hdrSize, 4);
	memcpy(hdr + 18, &w, 4);
	/* 负高度 = 自顶向下存储（我们的缓冲区就是自顶向下） */
	const int32_t nh = -h;
	memcpy(hdr + 22, &nh, 4);
	uint16_t planes = 1, bpp = 32;
	memcpy(hdr + 26, &planes, 2);
	memcpy(hdr + 28, &bpp, 2);
	memcpy(hdr + 34, &imgSize, 4);
	fwrite(hdr, 1, 54, f);
	fwrite(bgra.data(), 1, imgSize, f);
	fclose(f);
	return true;
}

} // namespace hfr