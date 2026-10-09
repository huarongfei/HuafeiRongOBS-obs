#include "HFRDeckRender.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
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
		   int &outH)
{
	const int boxW = std::max(1, (int)std::lround(sh.w * scale));
	const int boxH = std::max(1, (int)std::lround(sh.h * scale));
	GdiText g;
	if (!g.Create(boxW, boxH)) {
		return false;
	}
	int penY = 0;
	for (const Para &para : sh.paras) {
		/* 缩进：优先取文档 marL（EMU→px），否则按级别估算 */
		const int indent = (para.marL > 0) ? (int)std::lround(para.marL * scale)
						   : para.level * (int)std::lround(24 * ptScale);
		int maxFontPx = (int)std::lround(18 * ptScale);
		for (const Run &r : para.runs) {
			maxFontPx = std::max(maxFontPx, (int)std::lround(r.sizePt * ptScale));
		}
		const int lineH = (int)std::lround(maxFontPx * 1.22 * (para.lineSpacing > 0.1 ? para.lineSpacing : 1.0));
		for (const Run &r : para.runs) {
			if (r.text.empty()) {
				continue;
			}
			const std::wstring face = !r.ea.empty() ? r.ea : (!r.latin.empty() ? r.latin : deck.minorFont);
			const int fontPx = std::max(4, (int)std::lround((r.sizePt > 0 ? r.sizePt : 18.0) * ptScale));
			HFONT font = MakeFont(face, fontPx, r.bold, r.italic);
			HGDIOBJ oldFont = SelectObject(g.dc, font);
			SetTextColor(g.dc, RGB(255, 255, 255));

			std::wstring body = r.text;
			if (!para.bulletChar.empty()) {
				body = para.bulletChar + L" " + body;
			} else if (para.bullet) {
				body = std::wstring(L"• ") + body;
			}
			const int availW = std::max(16, boxW - indent - (int)std::lround(8 * scale));
			const std::vector<std::wstring> lines = WrapText(g.dc, body, availW);
			for (const std::wstring &line : lines) {
				if (!line.empty() && penY < boxH) {
					int x = indent + (int)std::lround(4 * scale);
					if (para.align == 1) {
						x = indent + std::max(0, (availW - MeasureRun(g.dc, line)) / 2);
					} else if (para.align == 2) {
						x = indent + std::max(0, availW - MeasureRun(g.dc, line));
					}
					TextOutW(g.dc, x, penY, line.c_str(), (int)line.size());
				}
				penY += lineH;
			}
			SelectObject(g.dc, oldFont);
			DeleteObject(font);
		}
		if (para.runs.empty()) {
			penY += lineH;
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

void AddShape(const Shape &sh, const Deck &deck, double scale, double ptScale, int dx, int dy, int shapeIndex,
	       RenderSlide &out)
{
	const int x = dx + (int)std::lround(sh.x * scale);
	const int y = dy + (int)std::lround(sh.y * scale);
	const int w = std::max(1, (int)std::lround(sh.w * scale));
	const int h = std::max(1, (int)std::lround(sh.h * scale));

	if (sh.kind == Shape::Kind::Group) {
		for (const Shape &c : sh.children) {
			AddShape(c, deck, scale, ptScale, x, y, shapeIndex, out);
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
			it.pix = std::move(decoded);
			it.texW = dw;
			it.texH = dh;
		} else {
			it.pix = sh.imageData; /* 退回：保留原始编码字节 */
			it.ext = sh.imageExt;
		}
		out.items.push_back(std::move(it));
		return;
	}
	/* 渐变填充：CPU 生成渐变位图（限制到合理分辨率） */
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
		const uint32_t gridColor = 0xFF9E9E9Eu;
		double yy = 0;
		for (size_t r = 0; r < sh.cells.size(); r++) {
			const int cy = y + (int)std::lround(yy / totalRow * (double)h);
			const int ch = (r < sh.rowH.size()) ? (int)std::lround(sh.rowH[r] / totalRow * (double)h)
							    : (int)((double)h / (double)sh.cells.size());
			double xx = 0;
			for (size_t c = 0; c < sh.cells[r].size(); c++) {
				const int cx = x + (int)std::lround(xx / totalCol * (double)w);
				const int cw = (c < sh.colW.size()) ? (int)std::lround(sh.colW[c] / totalCol * (double)w)
								    : (int)((double)w / (double)sh.cells[r].size());
				if (sh.cells[r][c].hasFill) {
					Item bg;
					bg.kind = ItemKind::Rect;
					bg.x = cx;
					bg.y = cy;
					bg.w = cw;
					bg.h = ch;
					bg.color = sh.cells[r][c].fill.argb;
					bg.shapeIndex = shapeIndex;
					out.items.push_back(std::move(bg));
				}
				if (!sh.cells[r][c].paras.empty()) {
					Shape cellShape;
					cellShape.kind = Shape::Kind::TextBox;
					cellShape.w = (int64_t)((double)cw / scale);
					cellShape.h = (int64_t)((double)ch / scale);
					cellShape.paras = sh.cells[r][c].paras;
					Item t;
					t.kind = ItemKind::Text;
					t.x = cx;
					t.y = cy;
					t.shapeIndex = shapeIndex;
					t.debug = sh.name;
					int tw = 0, th = 0;
					if (RasterizeText(cellShape, deck, scale, ptScale, t.pix, tw, th)) {
						t.texW = tw;
						t.texH = th;
						t.w = tw;
						t.h = th;
						out.items.push_back(std::move(t));
					}
				}
				if (c > 0) {
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
				xx += (c < sh.colW.size()) ? sh.colW[c] : 0;
			}
			if (r > 0) {
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
			yy += (r < sh.rowH.size()) ? sh.rowH[r] : 0;
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
		if (RasterizeText(sh, deck, scale, ptScale, it.pix, tw, th)) {
			it.texW = tw;
			it.texH = th;
			it.w = tw;
			it.h = th;
			out.items.push_back(std::move(it));
		}
	}
}

} // namespace

bool BuildSlide(const Deck &deck, int slideIndex, int targetW, int targetH, RenderSlide &out, std::wstring &err)
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
		AddShape(shapes[si], deck, scale, ptScale, dx, dy, (int)si, out);
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
			for (int y = it.y; y < it.y + it.h; y++) {
				for (int x = it.x; x < it.x + it.w; x++) {
					BlendPixel(bgra, W, x, y, it.color, 255);
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