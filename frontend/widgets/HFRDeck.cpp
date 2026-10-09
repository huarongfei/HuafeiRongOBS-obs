#include "HFRDeck.hpp"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <cstring>
#include <fstream>
#include <sstream>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cwchar>
#include <cwctype>

#include <zlib.h>

namespace hfr {
namespace {

/* ============================ zip 读取 ============================ */
uint16_t Rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t Rd32(const uint8_t *p) { return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); }

struct ZipEntry {
	std::string name;
	uint16_t method = 0;
	uint32_t compSize = 0;
	uint32_t uncompSize = 0;
	uint32_t localOff = 0;
};

struct Zip {
	std::vector<uint8_t> data;
	std::map<std::string, ZipEntry> entries;

	bool Open(const std::wstring &path)
	{
		std::ifstream f(path, std::ios::binary);
		if (!f) {
			return false;
		}
		f.seekg(0, std::ios::end);
		const std::streamoff n = f.tellg();
		f.seekg(0, std::ios::beg);
		if (n <= 0) {
			return false;
		}
		data.resize((size_t)n);
		f.read((char *)data.data(), n);
		if (!f) {
			return false;
		}
		/* 找 EOCD */
		const size_t maxBack = std::min<size_t>(data.size(), 66000);
		size_t eocd = SIZE_MAX;
		for (size_t i = data.size() - 22; i + 22 <= data.size() && i >= data.size() - maxBack; i--) {
			if (Rd32(&data[i]) == 0x06054b50u) {
				eocd = i;
				break;
			}
			if (i == 0) {
				break;
			}
		}
		if (eocd == SIZE_MAX) {
			return false;
		}
		const uint16_t count = Rd16(&data[eocd + 10]);
		uint32_t off = Rd32(&data[eocd + 16]);
		for (uint16_t i = 0; i < count && off + 46 <= data.size(); i++) {
			if (Rd32(&data[off]) != 0x02014b50u) {
				break;
			}
			ZipEntry e;
			e.method = Rd16(&data[off + 10]);
			e.compSize = Rd32(&data[off + 20]);
			e.uncompSize = Rd32(&data[off + 24]);
			const uint16_t nlen = Rd16(&data[off + 28]);
			const uint16_t elen = Rd16(&data[off + 30]);
			const uint16_t clen = Rd16(&data[off + 32]);
			e.localOff = Rd32(&data[off + 42]);
			if (off + 46 + nlen > data.size()) {
				break;
			}
			e.name.assign((const char *)&data[off + 46], nlen);
			entries[e.name] = e;
			off += 46u + nlen + elen + clen;
		}
		return !entries.empty();
	}

	bool Read(const std::string &name, std::vector<uint8_t> &out) const
	{
		auto it = entries.find(name);
		if (it == entries.end()) {
			return false;
		}
		const ZipEntry &e = it->second;
		if (e.localOff + 30 > data.size() || Rd32(&data[e.localOff]) != 0x04034b50u) {
			return false;
		}
		const uint16_t nlen = Rd16(&data[e.localOff + 26]);
		const uint16_t elen = Rd16(&data[e.localOff + 28]);
		const size_t start = e.localOff + 30u + nlen + elen;
		if (start + e.compSize > data.size()) {
			return false;
		}
		if (e.method == 0) {
			out.assign(data.begin() + start, data.begin() + start + e.compSize);
			return true;
		}
		if (e.method != 8) {
			return false;
		}
		out.assign(e.uncompSize ? e.uncompSize : 1, 0);
		z_stream zs;
		memset(&zs, 0, sizeof(zs));
		if (inflateInit2(&zs, -15) != Z_OK) {
			return false;
		}
		zs.next_in = (Bytef *)&data[start];
		zs.avail_in = e.compSize;
		zs.next_out = (Bytef *)out.data();
		zs.avail_out = (uInt)out.size();
		int rc = inflate(&zs, Z_FINISH);
		const size_t produced = out.size() - zs.avail_out;
		inflateEnd(&zs);
		out.resize(produced);
		return rc == Z_STREAM_END || produced == e.uncompSize;
	}

	bool Has(const std::string &n) const { return entries.count(n) != 0; }

	std::vector<std::string> NamesWithPrefix(const std::string &prefix) const
	{
		std::vector<std::string> r;
		for (const auto &kv : entries) {
			if (kv.first.compare(0, prefix.size(), prefix) == 0) {
				r.push_back(kv.first);
			}
		}
		return r;
	}
};

/* ============================ 迷你 XML ============================ */
struct XNode {
	std::wstring name;
	std::map<std::wstring, std::wstring> attrs;
	std::wstring text;
	std::vector<XNode> kids;

	const XNode *Child(const std::wstring &localName) const
	{
		for (const auto &k : kids) {
			if (LocalName(k.name) == localName) {
				return &k;
			}
		}
		return nullptr;
	}
	std::vector<const XNode *> Children(const std::wstring &localName) const
	{
		std::vector<const XNode *> r;
		for (const auto &k : kids) {
			if (LocalName(k.name) == localName) {
				r.push_back(&k);
			}
		}
		return r;
	}
	const std::wstring *Attr(const std::wstring &localName) const
	{
		/* 1) 优先关系引用（r:id 等）——同一元素常同时有 id 与 r:id */
		{
			auto it = attrs.find(L"r:" + localName);
			if (it != attrs.end()) {
				return &it->second;
			}
		}
		/* 2) 精确同名 */
		{
			auto it = attrs.find(localName);
			if (it != attrs.end()) {
				return &it->second;
			}
		}
		/* 3) 任意前缀的本地名匹配 */
		for (const auto &kv : attrs) {
			if (LocalName(kv.first) == localName) {
				return &kv.second;
			}
		}
		return nullptr;
	}

	static std::wstring LocalName(const std::wstring &n)
	{
		const size_t c = n.find(L':');
		return c == std::wstring::npos ? n : n.substr(c + 1);
	}
};

/* 宽 → UTF-8（避免 wchar_t→char 的迭代器转换告警/截断） */
std::string Narrow(const std::wstring &w)
{
	if (w.empty()) {
		return std::string();
	}
	const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s((size_t)n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
	return s;
}

std::wstring Utf8ToW(const std::string &s)
{
	if (s.empty()) {
		return std::wstring();
	}
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	std::wstring w((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
	return w;
}

void DecodeEntities(std::wstring &s)
{
	struct Rep {
		const wchar_t *from;
		const wchar_t *to;
	};
	static const Rep reps[] = {{L"&lt;", L"<"}, {L"&gt;", L">"}, {L"&quot;", L"\""}, {L"&apos;", L"'"}, {L"&amp;", L"&"}};
	for (const auto &r : reps) {
		size_t p = 0;
		const size_t fl = wcslen(r.from);
		while ((p = s.find(r.from, p)) != std::wstring::npos) {
			s.replace(p, fl, r.to);
			p += wcslen(r.to);
		}
	}
	/* 数字实体 */
	size_t p = 0;
	while ((p = s.find(L"&#", p)) != std::wstring::npos) {
		const size_t semi = s.find(L';', p);
		if (semi == std::wstring::npos) {
			break;
		}
		const std::wstring num = s.substr(p + 2, semi - p - 2);
		wchar_t ch = 0;
		try {
			ch = (wchar_t)std::stoi(num[0] == L'x' || num[0] == L'X' ? num.substr(1) : num, nullptr,
						num[0] == L'x' || num[0] == L'X' ? 16 : 10);
		} catch (...) {
			ch = 0;
		}
		if (ch) {
			std::wstring r(1, ch);
			s.replace(p, semi - p + 1, r);
			p += 1;
		} else {
			p = semi + 1;
		}
	}
}

class XmlParser {
public:
	XmlParser(const std::wstring &s) : src(s) {}

	bool Parse(XNode &out)
	{
		SkipProlog();
		if (!ParseElement(out)) {
			return false;
		}
		return true;
	}

private:
	const std::wstring &src;
	size_t i = 0;

	void SkipWs()
	{
		while (i < src.size() && (src[i] == L' ' || src[i] == L'\t' || src[i] == L'\r' || src[i] == L'\n')) {
			i++;
		}
	}
	void SkipProlog()
	{
		for (;;) {
			SkipWs();
			if (i + 1 < src.size() && src[i] == L'<' && src[i + 1] == L'?') {
				const size_t e = src.find(L"?>", i);
				i = (e == std::wstring::npos) ? src.size() : e + 2;
				continue;
			}
			if (i + 3 < src.size() && src.compare(i, 4, L"<!--") == 0) {
				const size_t e = src.find(L"-->", i);
				i = (e == std::wstring::npos) ? src.size() : e + 3;
				continue;
			}
			if (i + 8 < src.size() && src.compare(i, 9, L"<!DOCTYPE") == 0) {
				const size_t e = src.find(L'>', i);
				i = (e == std::wstring::npos) ? src.size() : e + 1;
				continue;
			}
			break;
		}
	}
	std::wstring ReadName()
	{
		const size_t s = i;
		while (i < src.size() && (iswalnum(src[i]) || src[i] == L'_' || src[i] == L'-' || src[i] == L':' || src[i] == L'.')) {
			i++;
		}
		return src.substr(s, i - s);
	}
	bool ParseElement(XNode &node)
	{
		SkipWs();
		if (i >= src.size() || src[i] != L'<') {
			return false;
		}
		i++;
		node.name = ReadName();
		/* 属性 */
		for (;;) {
			SkipWs();
			if (i >= src.size()) {
				return false;
			}
			if (src[i] == L'/' && i + 1 < src.size() && src[i + 1] == L'>') {
				i += 2;
				return true;
			}
			if (src[i] == L'>') {
				i++;
				break;
			}
			std::wstring an = ReadName();
			SkipWs();
			std::wstring av;
			if (i < src.size() && src[i] == L'=') {
				i++;
				SkipWs();
				if (i < src.size() && (src[i] == L'"' || src[i] == L'\'')) {
					const wchar_t q = src[i++];
					const size_t s = i;
					while (i < src.size() && src[i] != q) {
						i++;
					}
					av = src.substr(s, i - s);
					if (i < src.size()) {
						i++;
					}
					DecodeEntities(av);
				}
			}
			if (!an.empty()) {
				node.attrs[an] = av;
			}
		}
		/* 子节点/文本 */
		std::wstring text;
		for (;;) {
			const size_t lt = src.find(L'<', i);
			if (lt == std::wstring::npos) {
				text += src.substr(i);
				i = src.size();
				break;
			}
			text += src.substr(i, lt - i);
			i = lt;
			if (i + 3 < src.size() && src.compare(i, 4, L"<!--") == 0) {
				const size_t e = src.find(L"-->", i);
				i = (e == std::wstring::npos) ? src.size() : e + 3;
				continue;
			}
			if (i + 8 < src.size() && src.compare(i, 9, L"<![CDATA[") == 0) {
				const size_t e = src.find(L"]]>", i);
				if (e == std::wstring::npos) {
					i = src.size();
					break;
				}
				text += src.substr(i + 9, e - i - 9);
				i = e + 3;
				continue;
			}
			if (i + 1 < src.size() && src[i + 1] == L'/') {
				const size_t e = src.find(L'>', i);
				i = (e == std::wstring::npos) ? src.size() : e + 1;
				break;
			}
			XNode kid;
			if (!ParseElement(kid)) {
				break;
			}
			node.kids.push_back(std::move(kid));
		}
		node.text = text;
		DecodeEntities(node.text);
		return true;
	}
};

/* ============================ 工具 ============================ */
bool ParseXml(const std::vector<uint8_t> &bytes, XNode &out)
{
	std::wstring w = Utf8ToW(std::string((const char *)bytes.data(), bytes.size()));
	/* 去 BOM */
	if (!w.empty() && w[0] == 0xFEFF) {
		w.erase(0, 1);
	}
	XmlParser p(w);
	return p.Parse(out);
}

int64_t ToI64(const std::wstring *s, int64_t def = 0)
{
	if (!s || s->empty()) {
		return def;
	}
	try {
		return (int64_t)_wtoi64(s->c_str());
	} catch (...) {
		return def;
	}
}

double ToD(const std::wstring *s, double def = 0.0)
{
	if (!s || s->empty()) {
		return def;
	}
	return _wtof(s->c_str());
}

Color ParseColor(const XNode *fillNode, const std::map<std::wstring, std::wstring> &theme)
{
	Color c;
	if (!fillNode) {
		return c;
	}
	if (const XNode *srgb = fillNode->Child(L"srgbClr")) {
		if (const std::wstring *v = srgb->Attr(L"val")) {
			uint32_t rgb = (uint32_t)wcstoul(v->c_str(), nullptr, 16);
			c.argb = 0xFF000000u | (rgb & 0xFFFFFFu);
			c.valid = true;
			return c;
		}
	}
	if (const XNode *sc = fillNode->Child(L"schemeClr")) {
		if (const std::wstring *v = sc->Attr(L"val")) {
			auto it = theme.find(*v);
			if (it != theme.end()) {
				uint32_t rgb = (uint32_t)wcstoul(it->second.c_str(), nullptr, 16);
				c.argb = 0xFF000000u | (rgb & 0xFFFFFFu);
				c.valid = true;
			}
		}
	}
	if (const XNode *gf = fillNode->Child(L"gradFill")) {
		if (const XNode *lst = gf->Child(L"gsLst")) {
			if (const XNode *gs = lst->Child(L"gs")) {
				Color g = ParseColor(gs, theme);
				if (g.valid) {
					return g;
				}
			}
		}
	}
	if (const XNode *pc = fillNode->Child(L"prstClr")) {
		if (const std::wstring *v = pc->Attr(L"val")) {
			if (*v == L"white") {
				c.argb = 0xFFFFFFFFu;
				c.valid = true;
			} else if (*v == L"black") {
				c.argb = 0xFF000000u;
				c.valid = true;
			}
		}
	}
	return c;
}

void ParseTextBody(const XNode *txBody, Shape &sh, const std::map<std::wstring, std::wstring> &theme)
{
	if (!txBody) {
		return;
	}
	for (const XNode *p : txBody->Children(L"p")) {
		Para para;
		if (const XNode *ppr = p->Child(L"pPr")) {
			if (const std::wstring *a = ppr->Attr(L"algn")) {
				if (*a == L"ctr") {
					para.align = 1;
				} else if (*a == L"r") {
					para.align = 2;
				} else if (*a == L"just") {
					para.align = 3;
				}
			}
			para.level = (int)ToI64(ppr->Attr(L"lvl"));
			if (const XNode *bc = ppr->Child(L"buChar")) {
				if (const std::wstring *ch = bc->Attr(L"char")) {
					para.bulletChar = *ch;
					para.bullet = true;
					para.bulletExplicit = true;
				}
			}
			if (ppr->Child(L"buNone")) {
				para.bullet = false;
				para.bulletChar.clear();
				para.bulletExplicit = true;
			}
			if (ppr->Child(L"buAutoNum")) {
				para.bullet = true;
				para.bulletExplicit = true;
			}
			if (const std::wstring *ml = ppr->Attr(L"marL")) {
				para.marL = (int)_wtoi(ml->c_str());
			}
			if (const XNode *ln = ppr->Child(L"lnSpc")) {
				if (const XNode *pct = ln->Child(L"spcPct")) {
					para.lineSpacing = ToD(pct->Attr(L"val"), 100000.0) / 100000.0;
				}
			}
		}
		/* 段落级默认 run 属性 */
		for (const XNode *r : p->Children(L"r")) {
			Run run;
			if (const XNode *rpr = r->Child(L"rPr")) {
				if (const std::wstring *sz = rpr->Attr(L"sz")) {
					run.sizePt = _wtof(sz->c_str()) / 100.0;
					run.hasSize = true;
				}
				if (const std::wstring *b = rpr->Attr(L"b")) {
					run.bold = (*b == L"1" || *b == L"true");
				}
				if (const std::wstring *it2 = rpr->Attr(L"i")) {
					run.italic = (*it2 == L"1" || *it2 == L"true");
				}
				if (const std::wstring *u = rpr->Attr(L"u")) {
					run.underline = (*u != L"none");
				}
				run.color = ParseColor(rpr->Child(L"solidFill"), theme);
				if (const XNode *latin = rpr->Child(L"latin")) {
					if (const std::wstring *tf = latin->Attr(L"typeface")) {
						run.latin = *tf;
					}
				}
				if (const XNode *ea = rpr->Child(L"ea")) {
					if (const std::wstring *tf = ea->Attr(L"typeface")) {
						run.ea = *tf;
					}
				}
			}
			if (const XNode *t = r->Child(L"t")) {
				run.text = t->text;
			}
			if (!run.text.empty()) {
				para.runs.push_back(run);
			}
		}
		if (const XNode *fld = p->Child(L"fld")) {
			Run run;
			run.text = L"[字段]";
			para.runs.push_back(run);
		}
		sh.paras.push_back(std::move(para));
	}
}

void ParseShapeTree(const XNode *spTree, std::vector<Shape> &out, const std::map<std::wstring, std::wstring> &theme);

/* 解析单个 lvlNpPr → LevelStyle */
void ParseLevelStyle(const XNode *lvlPr, LevelStyle &out, const std::map<std::wstring, std::wstring> &theme)
{
	if (!lvlPr) {
		return;
	}
	if (const std::wstring *ml = lvlPr->Attr(L"marL")) {
		out.marL = (int)_wtoi(ml->c_str());
	}
	if (const XNode *ln = lvlPr->Child(L"lnSpc")) {
		if (const XNode *pct = ln->Child(L"spcPct")) {
			out.lnSpcPct = ToD(pct->Attr(L"val"), 100000.0) / 100000.0;
		}
	}
	if (const XNode *bc = lvlPr->Child(L"buChar")) {
		if (const std::wstring *ch = bc->Attr(L"char")) {
			out.bulletChar = *ch;
		}
	}
	if (lvlPr->Child(L"buNone")) {
		out.bulletNone = true;
	}
	if (const XNode *dr = lvlPr->Child(L"defRPr")) {
		if (const std::wstring *sz = dr->Attr(L"sz")) {
			out.sizePt = _wtof(sz->c_str()) / 100.0;
		}
		if (const std::wstring *b = dr->Attr(L"b")) {
			out.bold = (*b == L"1" || *b == L"true");
		}
		if (const std::wstring *i2 = dr->Attr(L"i")) {
			out.italic = (*i2 == L"1" || *i2 == L"true");
		}
		out.color = ParseColor(dr->Child(L"solidFill"), theme);
	}
}

/* 解析母版 p:txStyles → title/body/other 各级样式 */
void ParseTxStyles(const XNode &masterRoot, Deck &deck, const std::map<std::wstring, std::wstring> &theme)
{
	const XNode *txs = masterRoot.Child(L"txStyles");
	if (!txs) {
		return;
	}
	struct Entry {
		const wchar_t *node;
		const wchar_t *key;
	};
	const Entry entries[] = {{L"titleStyle", L"title"}, {L"bodyStyle", L"body"}, {L"otherStyle", L"other"}};
	for (const Entry &e : entries) {
		const XNode *st = txs->Child(e.node);
		if (!st) {
			continue;
		}
		std::vector<LevelStyle> levels(9);
		for (const XNode &k : st->kids) {
			const std::wstring ln = XNode::LocalName(k.name);
			if (ln.size() < 5 || ln.compare(0, 3, L"lvl") != 0) {
				continue;
			}
			const int idx = _wtoi(ln.substr(3, 1).c_str()) - 1;
			if (idx < 0 || idx >= 9) {
				continue;
			}
			ParseLevelStyle(&k, levels[(size_t)idx], theme);
		}
		deck.textStyles[e.key] = levels;
	}
}

/* 递归收集一个 cTn 子树内的行为（p:cBhvr）：得到目标形状、效果类型与时长 */
void CollectBehaviors(const XNode *node, Slide &slide, double inheritedDurMs)
{
	if (!node) {
		return;
	}
	const std::wstring ln = XNode::LocalName(node->name);
	if (ln == L"cBhvr") {
		Anim a;
		a.durMs = inheritedDurMs;
		const XNode *bcTn = node->Child(L"cTn");
		if (bcTn) {
			a.durMs = ToD(bcTn->Attr(L"dur"), inheritedDurMs);
			if (const XNode *sc = bcTn->Child(L"stCondLst")) {
				if (const XNode *c0 = sc->Child(L"cond")) {
					a.delayMs = ToD(c0->Attr(L"delay"), 0.0);
				}
			}
		}
		if (const XNode *tgt = node->Child(L"tgtEl")) {
			if (const XNode *sp = tgt->Child(L"spTgt")) {
				if (const std::wstring *spid = sp->Attr(L"spid")) {
					a.shapeId = (int)_wtoi(spid->c_str());
				}
			}
		}
		/* 效果类型 */
		for (const XNode &k : node->kids) {
			const std::wstring kn = XNode::LocalName(k.name);
			if (kn == L"animEffect") {
				if (const std::wstring *t = k.Attr(L"transition")) {
					a.transition = *t;
				}
				if (const std::wstring *f = k.Attr(L"filter")) {
					a.filter = *f;
				}
			} else if (kn == L"anim" || kn == L"set" || kn == L"animMotion" || kn == L"animScale" ||
				   kn == L"animRot" || kn == L"animClr") {
				if (const std::wstring *an = k.Attr(L"attrName")) {
					a.filter = *an;
				} else if (a.filter.empty()) {
					a.filter = kn;
				}
			}
			if (kn == L"cTn") {
				if (const std::wstring *pc = k.Attr(L"presetClass")) {
					a.presetClass = *pc;
				}
				if (const std::wstring *pi = k.Attr(L"presetID")) {
					a.presetId = (int)_wtoi(pi->c_str());
				}
			}
		}
		if (a.shapeId > 0 && !slide.animSteps.empty()) {
			slide.animSteps.back().push_back(a);
		}
		/* 继续深入（同一行为里可能嵌套子时间） */
		if (const XNode *child = node->Child(L"childTnLst")) {
			CollectBehaviors(child, slide, a.durMs);
		}
		return;
	}
	/* 记录最近的 dur 上下文 */
	double dur = inheritedDurMs;
	if (ln == L"cTn") {
		dur = ToD(node->Attr(L"dur"), inheritedDurMs);
		if (const std::wstring *pc = node->Attr(L"presetClass")) {
			(void)pc;
		}
	}
	for (const XNode &k : node->kids) {
		CollectBehaviors(&k, slide, dur);
	}
}

/* 从备注页 XML 提取纯文本 */
std::wstring ExtractNotesText(const std::vector<uint8_t> &xml)
{
	if (xml.empty()) {
		return std::wstring();
	}
	XNode root;
	if (!ParseXml(xml, root)) {
		return std::wstring();
	}
	const XNode *cSld = root.Child(L"cSld");
	const XNode *spTree = cSld ? cSld->Child(L"spTree") : root.Child(L"spTree");
	if (!spTree) {
		return std::wstring();
	}
	std::wstring out;
	std::function<void(const XNode *)> walk = [&](const XNode *node) {
		if (!node) {
			return;
		}
		const std::wstring ln = XNode::LocalName(node->name);
		if (ln == L"sp" || ln == L"graphicFrame") {
			const XNode *tx = node->Child(L"txBody");
			if (tx) {
				for (const XNode *p : tx->Children(L"p")) {
					std::wstring line;
					for (const XNode *r : p->Children(L"r")) {
						if (const XNode *t = r->Child(L"t")) {
							line += t->text;
						}
					}
					if (!line.empty()) {
						if (!out.empty()) {
							out += L"\n";
						}
						out += line;
					}
				}
			}
		}
		for (const XNode &k : node->kids) {
			walk(&k);
		}
	};
	walk(spTree);
	return out;
}

/* 解析 p:transition（页间切换） */
void ParseTransition(const XNode &slideRoot, Slide &slide)
{
	const XNode *tr = slideRoot.Child(L"transition");
	if (!tr) {
		return;
	}
	/* spd: slow=1000ms, med=500ms, fast=250ms（PowerPoint 常规取值） */
	double ms = 500.0;
	if (const std::wstring *spd = tr->Attr(L"spd")) {
		if (*spd == L"slow") {
			ms = 1000.0;
		} else if (*spd == L"fast") {
			ms = 250.0;
		}
	}
	if (const std::wstring *adv = tr->Attr(L"advClick")) {
		slide.transAdvClick = (*adv != L"0" && *adv != L"false");
	}
	/* 第一个子元素即为类型 */
	for (const XNode &k : tr->kids) {
		const std::wstring ln = XNode::LocalName(k.name);
		if (ln == L"extLst" || ln == L"sndAc") {
			continue;
		}
		slide.transType = ln;
		if (const std::wstring *dir = k.Attr(L"dir")) {
			slide.transDir = *dir;
		}
		if (const std::wstring *spd2 = k.Attr(L"spd")) {
			if (*spd2 == L"slow") {
				ms = 1000.0;
			} else if (*spd2 == L"fast") {
				ms = 250.0;
			}
		}
		break;
	}
	slide.transMs = ms;
}

/* 解析 p:timing → 每个点击步骤一组动画 */
void ParseTiming(const XNode &slideRoot, Slide &slide)
{
	const XNode *timing = slideRoot.Child(L"timing");
	if (!timing) {
		return;
	}
	const XNode *tnLst = timing->Child(L"tnLst");
	if (!tnLst) {
		return;
	}
	/* 找到 seq（点击序列） */
	const XNode *seq = nullptr;
	{
		std::vector<const XNode *> stack{tnLst};
		while (!stack.empty() && !seq) {
			const XNode *n = stack.back();
			stack.pop_back();
			if (XNode::LocalName(n->name) == L"seq") {
				seq = n;
				break;
			}
			for (const XNode &k : n->kids) {
				stack.push_back(&k);
			}
		}
	}
	if (!seq) {
		return;
	}
	const XNode *seqChild = seq->Child(L"cTn") ? seq->Child(L"cTn")->Child(L"childTnLst") : nullptr;
	if (!seqChild) {
		return;
	}
	/* seqChild 的每个 p:par 子树 = 一步 */
	for (const XNode &step : seqChild->kids) {
		const std::wstring sn = XNode::LocalName(step.name);
		if (sn != L"par") {
			continue;
		}
		slide.animSteps.push_back({});
		CollectBehaviors(&step, slide, 500.0);
	}
	/* 清掉空步骤 */
	std::vector<std::vector<Anim>> cleaned;
	for (auto &s : slide.animSteps) {
		if (!s.empty()) {
			cleaned.push_back(std::move(s));
		}
	}
	slide.animSteps = std::move(cleaned);
}

/* 把样式表应用到幻灯片（占位符或普通文本框） */
void ApplyTextStyles(Slide &slide, const Deck &deck)
{
	const auto titleIt = deck.textStyles.find(L"title");
	const auto bodyIt = deck.textStyles.find(L"body");
	const auto otherIt = deck.textStyles.find(L"other");
	for (Shape &sh : slide.shapes) {
		const std::vector<LevelStyle> *style = nullptr;
		if (sh.isPlaceholder) {
			const std::wstring t = sh.phType;
			if (t == L"title" || t == L"ctrTitle") {
				style = (titleIt != deck.textStyles.end()) ? &titleIt->second : nullptr;
			} else if (t.empty() || t == L"body" || t == L"subTitle" || t == L"obj") {
				style = (bodyIt != deck.textStyles.end()) ? &bodyIt->second : nullptr;
			} else if (otherIt != deck.textStyles.end()) {
				style = &otherIt->second;
			}
		} else if (otherIt != deck.textStyles.end()) {
			style = &otherIt->second;
		}
		for (Para &p : sh.paras) {
			const LevelStyle *ls = nullptr;
			if (style && !style->empty()) {
				const size_t idx = (size_t)std::max(0, std::min<int>(p.level, (int)style->size() - 1));
				ls = &(*style)[idx];
			}
			if (ls) {
				if (!p.bulletExplicit) {
					if (!ls->bulletChar.empty()) {
						p.bulletChar = ls->bulletChar;
						p.bullet = true;
					} else if (ls->bulletNone) {
						p.bullet = false;
					}
				}
				if (p.marL == 0) {
					p.marL = ls->marL;
				}
				if (p.lineSpacing <= 0.1 && ls->lnSpcPct > 0.1) {
					p.lineSpacing = ls->lnSpcPct;
				}
			}
			for (Run &r : p.runs) {
				if (ls) {
					if (!r.hasSize && ls->sizePt > 0) {
						r.sizePt = ls->sizePt;
					}
					if (!r.color.valid && ls->color.valid) {
						r.color = ls->color;
					}
				}
			}
		}
	}
}

void ParseShapeCommon(const XNode *node, Shape &sh, const std::map<std::wstring, std::wstring> &theme)
{
	if (const XNode *nv = node->Child(L"nvSpPr")) {
		if (const XNode *cnv = nv->Child(L"cNvPr")) {
			if (const std::wstring *n = cnv->Attr(L"name")) {
				sh.name = *n;
			}
			if (const std::wstring *idv = cnv->Attr(L"id")) {
				sh.id = (int)_wtoi(idv->c_str());
			}
		}
		if (const XNode *ph = nv->Child(L"nvPr")) {
			if (const XNode *p = ph->Child(L"ph")) {
				sh.isPlaceholder = true;
				if (const std::wstring *t = p->Attr(L"type")) {
					sh.phType = *t;
				}
				if (const std::wstring *ix = p->Attr(L"idx")) {
					sh.phIdx = (int)_wtoi(ix->c_str());
				}
			}
		}
	}
	const XNode *spPr = node->Child(L"spPr");
	if (spPr) {
		if (const XNode *xfrm = spPr->Child(L"xfrm")) {
			if (const XNode *off = xfrm->Child(L"off")) {
				sh.x = ToI64(off->Attr(L"x"));
				sh.y = ToI64(off->Attr(L"y"));
			}
			if (const XNode *ext = xfrm->Child(L"ext")) {
				sh.w = ToI64(ext->Attr(L"cx"));
				sh.h = ToI64(ext->Attr(L"cy"));
			}
			sh.rot = ToI64(xfrm->Attr(L"rot"));
		}
		if (const XNode *g = spPr->Child(L"prstGeom")) {
			if (const std::wstring *p = g->Attr(L"prst")) {
				sh.prstGeom = *p;
			}
		}
		if (const XNode *f = spPr->Child(L"solidFill")) {
			sh.fill = ParseColor(f, theme);
			sh.hasFill = sh.fill.valid;
		}
		if (const XNode *gf = spPr->Child(L"gradFill")) {
			if (const XNode *lst = gf->Child(L"gsLst")) {
				for (const XNode *gs : lst->Children(L"gs")) {
					GradientStop st;
					st.pos = ToD(gs->Attr(L"pos"), 0.0) / 100000.0;
					st.color = ParseColor(gs, theme);
					if (st.color.valid) {
						sh.gradStops.push_back(st);
					}
				}
			}
			if (const XNode *lin = gf->Child(L"lin")) {
				sh.gradAngleDeg = ToD(lin->Attr(L"ang"), 5400000.0) / 60000.0;
			}
			if (!sh.gradStops.empty()) {
				sh.hasFill = true;
				sh.fill = sh.gradStops[0].color;
				sh.hasGradient = (sh.gradStops.size() >= 2);
			}
		}
		if (const XNode *ln = spPr->Child(L"ln")) {
			sh.line = ParseColor(ln, theme);
			sh.hasLine = sh.line.valid;
			if (const std::wstring *w = ln->Attr(L"w")) {
				sh.lineW = ToI64(w);
			}
		}
	}
}

void ParseShapeTree(const XNode *spTree, std::vector<Shape> &out, const std::map<std::wstring, std::wstring> &theme)
{
	if (!spTree) {
		return;
	}
	for (const XNode &k : spTree->kids) {
		const std::wstring ln = XNode::LocalName(k.name);
		if (ln == L"sp") {
			Shape sh;
			sh.kind = Shape::Kind::AutoShape;
			ParseShapeCommon(&k, sh, theme);
			const XNode *tx = k.Child(L"txBody");
			if (tx) {
				ParseTextBody(tx, sh, theme);
				if (!sh.paras.empty() && sh.prstGeom.empty()) {
					sh.kind = Shape::Kind::TextBox;
				}
			}
			if (!sh.prstGeom.empty() && sh.prstGeom != L"rect") {
				sh.kind = Shape::Kind::AutoShape;
			}
			out.push_back(std::move(sh));
		} else if (ln == L"pic") {
			Shape sh;
			sh.kind = Shape::Kind::Picture;
			ParseShapeCommon(&k, sh, theme);
			if (const XNode *bf = k.Child(L"blipFill")) {
				if (const XNode *blip = bf->Child(L"blip")) {
					if (const std::wstring *e = blip->Attr(L"embed")) {
						sh.imageRelId = *e;
					}
				}
			}
			out.push_back(std::move(sh));
		} else if (ln == L"graphicFrame") {
			Shape sh;
			ParseShapeCommon(&k, sh, theme);
			const XNode *gf = k.Child(L"xfrm");
			if (gf) {
				if (const XNode *off = gf->Child(L"off")) {
					sh.x = ToI64(off->Attr(L"x"));
					sh.y = ToI64(off->Attr(L"y"));
				}
				if (const XNode *ext = gf->Child(L"ext")) {
					sh.w = ToI64(ext->Attr(L"cx"));
					sh.h = ToI64(ext->Attr(L"cy"));
				}
			}
			if (k.Child(L"graphic")) {
				const XNode *g = k.Child(L"graphic");
				const XNode *gd = g->Child(L"graphicData");
				if (gd) {
					if (const XNode *tbl = gd->Child(L"tbl")) {
						sh.kind = Shape::Kind::Table;
						/* 列宽 */
						if (const XNode *grid = tbl->Child(L"tblGrid")) {
							for (const XNode *gc : grid->Children(L"gridCol")) {
								sh.colW.push_back((double)ToI64(gc->Attr(L"w")));
							}
						}
						/* 行高与单元格 */
						for (const XNode *tr : tbl->Children(L"tr")) {
							sh.rowH.push_back((double)ToI64(tr->Attr(L"h")));
							std::vector<TableCell> row;
							for (const XNode *tc : tr->Children(L"tc")) {
								TableCell cell;
								if (const std::wstring *hm = tc->Attr(L"hMerge")) {
									cell.merged = (*hm == L"1");
								}
								if (const XNode *tx = tc->Child(L"txBody")) {
									Shape tmp;
									ParseTextBody(tx, tmp, theme);
									cell.paras = tmp.paras;
								}
								if (const XNode *pr = tc->Child(L"tcPr")) {
									if (const XNode *sf = pr->Child(L"solidFill")) {
										cell.fill = ParseColor(sf, theme);
										cell.hasFill = cell.fill.valid;
									}
								}
								row.push_back(std::move(cell));
							}
							sh.cells.push_back(std::move(row));
						}
					} else if (gd->Child(L"chart")) {
						sh.kind = Shape::Kind::Chart;
					} else {
						sh.kind = Shape::Kind::Unknown;
					}
				}
			}
			out.push_back(std::move(sh));
		} else if (ln == L"grpSp") {
			Shape sh;
			sh.kind = Shape::Kind::Group;
			ParseShapeCommon(&k, sh, theme);
			if (const XNode *gsp = k.Child(L"grpSpPr")) {
				if (const XNode *xfrm = gsp->Child(L"xfrm")) {
					if (const XNode *off = xfrm->Child(L"off")) {
						sh.x = ToI64(off->Attr(L"x"));
						sh.y = ToI64(off->Attr(L"y"));
					}
					if (const XNode *ext = xfrm->Child(L"ext")) {
						sh.w = ToI64(ext->Attr(L"cx"));
						sh.h = ToI64(ext->Attr(L"cy"));
					}
				}
			}
			ParseShapeTree(&k, sh.children, theme);
			out.push_back(std::move(sh));
		} else if (ln == L"cxnSp") {
			Shape sh;
			sh.kind = Shape::Kind::AutoShape;
			ParseShapeCommon(&k, sh, theme);
			out.push_back(std::move(sh));
		}
	}
}

/* rels: 解析 ppt/slides/_rels/slideN.xml.rels → id -> target */
struct RelInfo {
	std::wstring target;
	std::wstring type;
};

void ParseRelsEx(const XNode &root, std::map<std::wstring, RelInfo> &out)
{
	for (const XNode *rel : root.Children(L"Relationship")) {
		const std::wstring *id = rel->Attr(L"Id");
		const std::wstring *tg = rel->Attr(L"Target");
		const std::wstring *ty = rel->Attr(L"Type");
		if (id && tg) {
			RelInfo ri;
			ri.target = *tg;
			if (ty) {
				ri.type = *ty;
			}
			out[*id] = ri;
		}
	}
}

void ParseRels(const XNode &root, std::map<std::wstring, std::wstring> &out)
{
	std::map<std::wstring, RelInfo> tmp;
	ParseRelsEx(root, tmp);
	for (const auto &kv : tmp) {
		out[kv.first] = kv.second.target;
	}
}

/* 占位符键：type|idx */
std::wstring PhKey(const Shape &s)
{
	return s.phType + L"|" + std::to_wstring(s.phIdx);
}

/* 合并占位符：后者有值才覆盖（版式常不含几何，需保留母版的） */
void MergePlaceholder(std::map<std::wstring, Shape> &map, const std::wstring &key, const Shape &s)
{
	auto it = map.find(key);
	if (it == map.end()) {
		map[key] = s;
		return;
	}
	Shape &dst = it->second;
	if (s.w > 0 && s.h > 0) {
		dst.x = s.x;
		dst.y = s.y;
		dst.w = s.w;
		dst.h = s.h;
	}
	if (s.hasFill) {
		dst.hasFill = true;
		dst.fill = s.fill;
	}
	if (!s.prstGeom.empty()) {
		dst.prstGeom = s.prstGeom;
	}
	if (!s.paras.empty()) {
		dst.paras = s.paras;
	}
}

/* 从一段 slideMaster/slideLayout XML 中取出占位符形状 */
void CollectPlaceholders(const std::vector<uint8_t> &xml, std::map<std::wstring, Shape> &out,
			 const std::map<std::wstring, std::wstring> &theme)
{
	if (xml.empty()) {
		return;
	}
	XNode root;
	if (!ParseXml(xml, root)) {
		return;
	}
	const XNode *cSld = root.Child(L"cSld");
	const XNode *spTree = cSld ? cSld->Child(L"spTree") : root.Child(L"spTree");
	std::vector<Shape> shapes;
	ParseShapeTree(spTree, shapes, theme);
	for (const Shape &s : shapes) {
		if (!s.isPlaceholder) {
			continue;
		}
		MergePlaceholder(out, PhKey(s), s);
		if (!s.phType.empty()) {
			/* 类型兜底键（部分文档只给 type 不给 idx） */
			MergePlaceholder(out, s.phType + L"|*", s);
		}
		if (s.phIdx >= 0) {
			/* 序号兜底键（幻灯片侧常只有 idx，没有 type） */
			MergePlaceholder(out, L"idx:" + std::to_wstring(s.phIdx), s);
		}
	}
}

std::string NormalizeTarget(const std::string &slidePart, const std::string &target)
{
	/* slidePart 例："ppt/slides/slide1.xml" */
	if (target.empty()) {
		return target;
	}
	if (target[0] == '/') {
		return target.substr(1);
	}
	std::string base = slidePart;
	const size_t slash = base.find_last_of('/');
	base = (slash == std::string::npos) ? std::string() : base.substr(0, slash + 1);
	std::string joined = base + target;
	/* 处理 ../ */
	std::vector<std::string> parts;
	std::stringstream ss(joined);
	std::string seg;
	while (std::getline(ss, seg, '/')) {
		if (seg == "..") {
			if (!parts.empty()) {
				parts.pop_back();
			}
		} else if (seg != "." && !seg.empty()) {
			parts.push_back(seg);
		}
	}
	std::string r;
	for (size_t i = 0; i < parts.size(); i++) {
		if (i) {
			r += "/";
		}
		r += parts[i];
	}
	return r;
}

} // namespace

bool LoadDeck(const std::wstring &pptxPath, Deck &out, std::wstring &err)
{
	Zip zip;
	if (!zip.Open(pptxPath)) {
		err = L"无法读取 pptx（zip 打开失败）";
		return false;
	}

	/* 主题：字体与配色 */
	std::map<std::wstring, std::wstring> theme;
	{
		std::vector<uint8_t> b;
		if (zip.Read("ppt/theme/theme1.xml", b)) {
			XNode root;
			if (ParseXml(b, root)) {
				if (const XNode *cs = root.Child(L"themeElements")) {
					if (const XNode *clr = cs->Child(L"clrScheme")) {
						for (const XNode &c : clr->kids) {
							const std::wstring name = XNode::LocalName(c.name);
							const XNode *v = c.kids.empty() ? nullptr : &c.kids[0];
							if (!v) {
								continue;
							}
							const std::wstring *val = v->Attr(L"val") ? v->Attr(L"val") : v->Attr(L"lastClr");
							if (val) {
								theme[name] = *val;
							}
						}
					}
					if (const XNode *fs = cs->Child(L"fontScheme")) {
						if (const XNode *mj = fs->Child(L"majorFont")) {
							if (const XNode *lt = mj->Child(L"latin")) {
								if (const std::wstring *tf = lt->Attr(L"typeface")) {
									out.majorFont = *tf;
								}
							}
						}
						if (const XNode *mn = fs->Child(L"minorFont")) {
							if (const XNode *lt = mn->Child(L"latin")) {
								if (const std::wstring *tf = lt->Attr(L"typeface")) {
									out.minorFont = *tf;
								}
							}
						}
					}
				}
			}
		}
	}
	out.themeColors = theme;

	/* 母版文本样式（标题/正文各级字号与项目符号） */
	{
		std::map<std::wstring, RelInfo> presRelsAll;
		{
			std::vector<uint8_t> prb;
			if (zip.Read("ppt/_rels/presentation.xml.rels", prb)) {
				XNode rr;
				if (ParseXml(prb, rr)) {
					ParseRelsEx(rr, presRelsAll);
				}
			}
		}
		std::string masterPart;
		for (const auto &kv : presRelsAll) {
			if (kv.second.type.find(L"slideMaster") != std::wstring::npos) {
				masterPart = NormalizeTarget("ppt/presentation.xml",
							      Narrow(kv.second.target));
				break;
			}
		}
		if (masterPart.empty()) {
			masterPart = "ppt/slideMasters/slideMaster1.xml";
		}
		std::vector<uint8_t> mb;
		if (zip.Read(masterPart, mb)) {
			XNode mr;
			if (ParseXml(mb, mr)) {
				ParseTxStyles(mr, out, theme);
			}
		}
		out.debugInfo += L"master=" + std::wstring(masterPart.begin(), masterPart.end()) + L" styles=" +
				 std::to_wstring(out.textStyles.size()) + L"\n";
	}

	/* presentation.xml：页面尺寸 + 幻灯片顺序 */
	std::vector<std::string> slideParts;
	size_t dbgRelsRead = 0, dbgRelsCount = 0, dbgNoRid = 0;
	size_t dbgSldIds = 0, dbgRelHit = 0, dbgSlidesUnreadable = 0, dbgSlidesUnparsed = 0, dbgNoSpTree = 0;
	std::wstring dbgFirstRelId, dbgFirstSldRid;
	{
		std::vector<uint8_t> b;
		if (!zip.Read("ppt/presentation.xml", b)) {
			err = L"缺少 ppt/presentation.xml（不是有效的 pptx）";
			return false;
		}
		XNode root;
		if (!ParseXml(b, root)) {
			err = L"presentation.xml 解析失败";
			return false;
		}
		if (const XNode *sz = root.Child(L"sldSz")) {
			out.slideW = ToI64(sz->Attr(L"cx"), out.slideW);
			out.slideH = ToI64(sz->Attr(L"cy"), out.slideH);
		}
		std::map<std::wstring, std::wstring> presRels;
		std::vector<uint8_t> rb;
		if (zip.Read("ppt/_rels/presentation.xml.rels", rb)) {
			dbgRelsRead = 1;
			XNode rr;
			if (ParseXml(rb, rr)) {
				ParseRels(rr, presRels);
				dbgRelsCount = presRels.size();
				if (!presRels.empty()) {
					dbgFirstRelId = presRels.begin()->first;
				}
			}
		}
		if (const XNode *lst = root.Child(L"sldIdLst")) {
			for (const XNode *sid : lst->Children(L"sldId")) {
				dbgSldIds++;
				const std::wstring *rid = sid->Attr(L"id");
				if (!rid) {
					dbgNoRid++;
					continue;
				}
				if (dbgFirstSldRid.empty()) {
					dbgFirstSldRid = *rid;
				}
				auto it = presRels.find(*rid);
				if (it == presRels.end()) {
					continue;
				}
				dbgRelHit++;
				slideParts.push_back(NormalizeTarget("ppt/presentation.xml", Narrow(it->second)));
			}
		}
	}

	/* 逐个 slide */
	for (const std::string &part : slideParts) {
		std::vector<uint8_t> b;
		if (!zip.Read(part, b)) {
			dbgSlidesUnreadable++;
			continue;
		}
		XNode root;
		if (!ParseXml(b, root)) {
			dbgSlidesUnparsed++;
			continue;
		}
		Slide slide;
		/* rels → 图片 */
		std::map<std::wstring, std::wstring> rels;
		{
			const std::string slash = part.substr(0, part.find_last_of('/'));
			const std::string fileName = part.substr(part.find_last_of('/') + 1);
			std::vector<uint8_t> rb;
			if (zip.Read(slash + "/_rels/" + fileName + ".rels", rb)) {
				XNode rr;
				if (ParseXml(rb, rr)) {
					ParseRels(rr, rels);
				}
			}
		}
		const XNode *spTree = root.Child(L"cSld") ? root.Child(L"cSld")->Child(L"spTree") : root.Child(L"spTree");
		if (!spTree) {
			dbgNoSpTree++;
		}
		ParseShapeTree(spTree, slide.shapes, theme);

		/* —— 版式/母版占位符继承（真实稿件的正文几乎都在占位符里） —— */
		{
			std::map<std::wstring, Shape> phMap;
			/* slide → slideLayout */
			std::string layoutPart;
			{
				const std::string slashp = part.substr(0, part.find_last_of('/'));
				const std::string fn = part.substr(part.find_last_of('/') + 1);
				std::vector<uint8_t> rb;
				if (zip.Read(slashp + "/_rels/" + fn + ".rels", rb)) {
					XNode rr;
					std::map<std::wstring, RelInfo> info;
					if (ParseXml(rb, rr)) {
						ParseRelsEx(rr, info);
						for (const auto &kv : info) {
							if (kv.second.type.find(L"slideLayout") != std::wstring::npos) {
								layoutPart = NormalizeTarget(part, Narrow(kv.second.target));
								break;
							}
						}
					}
				}
			}
			/* slideLayout → slideMaster（先母版后版式覆盖） */
			if (!layoutPart.empty()) {
				const std::string lslash = layoutPart.substr(0, layoutPart.find_last_of('/'));
				const std::string lfn = layoutPart.substr(layoutPart.find_last_of('/') + 1);
				std::vector<uint8_t> rb;
				if (zip.Read(lslash + "/_rels/" + lfn + ".rels", rb)) {
					XNode rr;
					std::map<std::wstring, RelInfo> info;
					if (ParseXml(rb, rr)) {
						ParseRelsEx(rr, info);
						for (const auto &kv : info) {
							if (kv.second.type.find(L"slideMaster") != std::wstring::npos) {
								const std::string mp = NormalizeTarget(
									layoutPart, Narrow(kv.second.target));
								std::vector<uint8_t> mb;
								if (zip.Read(mp, mb)) {
									CollectPlaceholders(mb, phMap, theme);
								}
								break;
							}
						}
					}
				}
			}
			std::vector<uint8_t> lb;
			if (!layoutPart.empty() && zip.Read(layoutPart, lb)) {
				CollectPlaceholders(lb, phMap, theme);
			}
			{
				std::wstring dbg = L"slide" + std::to_wstring(out.slides.size() + 1) + L": layout=" +
						   std::wstring(layoutPart.begin(), layoutPart.end()) + L" phMap=" +
						   std::to_wstring(phMap.size());
				for (const auto &kv : phMap) {
					dbg += L" [" + kv.first + L"]";
				}
				out.debugInfo += dbg + L"\n";
			}
			/* 应用：位置/尺寸、填充、几何 */
			for (Shape &sh : slide.shapes) {
				if (!sh.isPlaceholder) {
					continue;
				}
				std::vector<std::wstring> keys;
				keys.push_back(PhKey(sh));
				if (!sh.phType.empty()) {
					keys.push_back(sh.phType + L"|*");
				}
				if (sh.phIdx >= 0) {
					keys.push_back(L"idx:" + std::to_wstring(sh.phIdx));
					keys.push_back(L"|" + std::to_wstring(sh.phIdx));
					keys.push_back(L"body|" + std::to_wstring(sh.phIdx));
				}
				/* 择优：优先"带几何"的候选，避免空条目挡住母版回退 */
				const Shape *best = nullptr;
				for (const std::wstring &k : keys) {
					auto it2 = phMap.find(k);
					if (it2 == phMap.end()) {
						continue;
					}
					if (!best) {
						best = &it2->second;
					}
					if (it2->second.w > 0 && it2->second.h > 0) {
						best = &it2->second;
						break;
					}
				}
				if (!best) {
					out.debugInfo += L"   未命中键 " + PhKey(sh) + L"\n";
					continue;
				}
				const Shape &lay = *best;
				const bool noGeom = (sh.w <= 0 || sh.h <= 0);
				if (noGeom) {
					sh.x = lay.x;
					sh.y = lay.y;
					sh.w = lay.w;
					sh.h = lay.h;
				}
				if (!sh.hasFill && lay.hasFill) {
					sh.hasFill = true;
					sh.fill = lay.fill;
				}
				if (sh.prstGeom.empty() && !lay.prstGeom.empty()) {
					sh.prstGeom = lay.prstGeom;
				}
			}
		}

		/* 解析图片数据 */
		for (Shape &sh : slide.shapes) {
			if (sh.imageRelId.empty()) {
				continue;
			}
			auto it = rels.find(sh.imageRelId);
			if (it == rels.end()) {
				continue;
			}
			const std::string target = NormalizeTarget(part, Narrow(it->second));
			std::vector<uint8_t> img;
			if (zip.Read(target, img)) {
				sh.imageData = std::move(img);
				sh.hasImage = !sh.imageData.empty();
				const size_t d = target.find_last_of('.');
				if (d != std::string::npos) {
					sh.imageExt = Utf8ToW(target.substr(d + 1));
				}
			}
		}
		/* 备注（notesSlide） */
		{
			const std::string slashn = part.substr(0, part.find_last_of('/'));
			const std::string fnn = part.substr(part.find_last_of('/') + 1);
			std::vector<uint8_t> rb;
			if (zip.Read(slashn + "/_rels/" + fnn + ".rels", rb)) {
				XNode rr;
				std::map<std::wstring, RelInfo> info;
				if (ParseXml(rb, rr)) {
					ParseRelsEx(rr, info);
					for (const auto &kv : info) {
						if (kv.second.type.find(L"notesSlide") != std::wstring::npos) {
							const std::string np =
								NormalizeTarget(part, Narrow(kv.second.target));
							std::vector<uint8_t> nb;
							if (zip.Read(np, nb)) {
								slide.notes = ExtractNotesText(nb);
							}
							break;
						}
					}
				}
			}
		}

		ApplyTextStyles(slide, out);
		/* 页间切换 + 动画 */
		ParseTransition(root, slide);
		ParseTiming(root, slide);
		for (auto &step : slide.animSteps) {
			for (Anim &a : step) {
				for (size_t si = 0; si < slide.shapes.size(); si++) {
					if (slide.shapes[si].id == a.shapeId) {
						a.shapeIndex = (int)si;
						break;
					}
				}
			}
		}
		out.debugInfo += L"slide" + std::to_wstring(out.slides.size() + 1) + L": animSteps=" +
				 std::to_wstring(slide.animSteps.size()) + L" trans=" + slide.transType +
				 (slide.transDir.empty() ? L"" : (L"/" + slide.transDir)) + L"\n";
		out.slides.push_back(std::move(slide));
	}

	if (out.slides.empty()) {
		wchar_t buf[256];
		swprintf(buf, 256, L"未解析到任何幻灯片（rels=%zu 条数=%zu 首Id=%ls | sldId=%zu 首rid=%ls 无rid=%zu 命中=%zu parts=%zu）",
			 dbgRelsRead, dbgRelsCount, dbgFirstRelId.c_str(), dbgSldIds, dbgFirstSldRid.c_str(), dbgNoRid, dbgRelHit,
			 slideParts.size());
		err = buf;
		return false;
	}
	return true;
}

std::string DeckSummary(const Deck &deck)
{
	std::ostringstream o;
	o << "slideSize=" << deck.slideW << "x" << deck.slideH << " EMU ("
	  << (double)deck.slideW / 914400.0 << "in x " << (double)deck.slideH / 914400.0 << "in)\n";
	o << "slides=" << deck.slides.size() << " themeColors=" << deck.themeColors.size() << " majorFont="
	  << Narrow(deck.majorFont) << "\n";
	for (size_t i = 0; i < deck.slides.size(); i++) {
		const Slide &s = deck.slides[i];
		o << "  slide " << (i + 1) << ": shapes=" << s.shapes.size() << "\n";
		int shown = 0;
		for (const Shape &sh : s.shapes) {
			if (shown++ >= 12) {
				o << "    ...\n";
				break;
			}
			const char *kind = "?";
			switch (sh.kind) {
			case Shape::Kind::AutoShape: kind = "shape"; break;
			case Shape::Kind::TextBox: kind = "text"; break;
			case Shape::Kind::Picture: kind = "pic"; break;
			case Shape::Kind::Group: kind = "group"; break;
			case Shape::Kind::Table: kind = "table"; break;
			case Shape::Kind::Chart: kind = "chart"; break;
			case Shape::Kind::Media: kind = "media"; break;
			default: kind = "unknown"; break;
			}
			o << "    [" << kind << "] geom=" << Narrow(sh.prstGeom)
			  << " pos=(" << sh.x << "," << sh.y << ") size=(" << sh.w << "x" << sh.h << ")"
			  << (sh.isPlaceholder ? " ph=" + Narrow(sh.phType) + ":" +
							std::to_string(sh.phIdx)
					      : std::string())
			  << (sh.hasFill ? " fill=1" : " fill=0")
			  << (sh.hasImage ? " img=1" : "") << (sh.imageData.empty() ? "" : " imgBytes=" + std::to_string(sh.imageData.size()));
			size_t total = 0;
			for (const Para &p : sh.paras) {
				for (const Run &r : p.runs) {
					total += r.text.size();
				}
			}
			if (total) {
				o << " runs=" << sh.paras.size() << " chars=" << total;
			}
			if (sh.kind == Shape::Kind::Table) {
				o << " tableCols=" << sh.colW.size() << " rows=" << sh.rowH.size()
				  << " cellRows=" << sh.cells.size();
				if (!sh.cells.empty()) {
					o << " cellCols0=" << sh.cells[0].size();
				}
			}
			o << "\n";
		}
	}
	return o.str();
}

} // namespace hfr