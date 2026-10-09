/* 独立验证：解析 pptx + CPU 渲染为 BMP（不依赖 OBS） */
#include "HFRDeck.hpp"
#include "HFRDeckRender.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>
#include <cstdlib>
#include <string>

static std::string ToUtf8(const std::wstring &w)
{
	if (w.empty()) {
		return std::string();
	}
	const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s((size_t)n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
	return s;
}

/* ---- pdfium 参考渲染（用于"我们的引擎 vs LibreOffice 官方渲染"保真对比） ---- */
typedef void *FPDF_DOCUMENT;
typedef void *FPDF_PAGE;
typedef void *FPDF_BITMAP;

static bool RenderPdfPage(const std::wstring &pdfiumDll, const std::wstring &pdfPath, int pageIdx, int W, int H,
			  const std::wstring &bmpOut)
{
	HMODULE lib = LoadLibraryExW(pdfiumDll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!lib) {
		lib = LoadLibraryW(pdfiumDll.c_str());
	}
	if (!lib) {
		printf("pdfium load failed (%lu)\n", (unsigned long)GetLastError());
		return false;
	}
	auto init = (void (*)())GetProcAddress(lib, "FPDF_InitLibrary");
	auto loadMem = (FPDF_DOCUMENT(*)(const void *, size_t, const char *))GetProcAddress(lib, "FPDF_LoadMemDocument64");
	auto pageCount = (int (*)(FPDF_DOCUMENT))GetProcAddress(lib, "FPDF_GetPageCount");
	auto loadPage = (FPDF_PAGE(*)(FPDF_DOCUMENT, int))GetProcAddress(lib, "FPDF_LoadPage");
	auto closePage = (void (*)(FPDF_PAGE))GetProcAddress(lib, "FPDF_ClosePage");
	auto bmpCreate = (FPDF_BITMAP(*)(int, int, int, void *, int))GetProcAddress(lib, "FPDFBitmap_CreateEx");
	auto bmpFill = (void (*)(FPDF_BITMAP, int, int, int, int, unsigned long))GetProcAddress(lib, "FPDFBitmap_FillRect");
	auto bmpDestroy = (void (*)(FPDF_BITMAP))GetProcAddress(lib, "FPDFBitmap_Destroy");
	auto render = (void (*)(FPDF_BITMAP, FPDF_PAGE, int, int, int, int, int, int))GetProcAddress(lib,
											      "FPDF_RenderPageBitmap");
	if (!init || !loadMem || !loadPage || !bmpCreate || !render) {
		printf("pdfium symbols missing\n");
		return false;
	}
	init();
	FILE *f = nullptr;
	if (_wfopen_s(&f, pdfPath.c_str(), L"rb") != 0 || !f) {
		printf("pdf open failed\n");
		return false;
	}
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	std::vector<uint8_t> data((size_t)n);
	size_t rd = fread(data.data(), 1, (size_t)n, f);
	fclose(f);
	FPDF_DOCUMENT doc = loadMem(data.data(), rd, nullptr);
	if (!doc) {
		printf("pdf load failed\n");
		return false;
	}
	const int pages = pageCount ? pageCount(doc) : 0;
	FPDF_PAGE page = loadPage(doc, pageIdx);
	if (!page) {
		printf("pdf page %d missing (pages=%d)\n", pageIdx, pages);
		return false;
	}
	std::vector<uint8_t> pix((size_t)W * H * 4, 0xFF);
	FPDF_BITMAP bmp = bmpCreate(W, H, 4, pix.data(), W * 4);
	bmpFill(bmp, 0, 0, W, H, 0xFFFFFFFFu);
	render(bmp, page, 0, 0, W, H, 0, 1);
	bmpDestroy(bmp);
	closePage(page);
	hfr::WriteBmp32(bmpOut, W, H, pix);
	printf("PDF OK pages=%d page=%d -> %s\n", pages, pageIdx + 1, ToUtf8(bmpOut).c_str());
	return true;
}

int wmain(int argc, wchar_t **argv)
{
	SetConsoleOutputCP(CP_UTF8);
	if (argc < 2) {
		printf("usage: hfr_deck_dump <file.pptx> [--bmp out.bmp] [--slide N] [--size WxH]\n");
		return 2;
	}
	const std::wstring path = argv[1];
	std::wstring bmpOut;
	int slide = 0;
	int W = 1280, H = 720;
	std::wstring pdfRef, pdfiumDll;
	for (int i = 2; i < argc; i++) {
		const std::wstring a = argv[i];
		if (a == L"--bmp" && i + 1 < argc) {
			bmpOut = argv[++i];
		} else if (a == L"--slide" && i + 1 < argc) {
			slide = _wtoi(argv[++i]);
		} else if (a == L"--pdfref" && i + 1 < argc) {
			pdfRef = argv[++i];
		} else if (a == L"--pdfium" && i + 1 < argc) {
			pdfiumDll = argv[++i];
		} else if (a == L"--size" && i + 1 < argc) {
			const std::wstring s = argv[++i];
			const size_t x = s.find(L'x');
			if (x != std::wstring::npos) {
				W = _wtoi(s.substr(0, x).c_str());
				H = _wtoi(s.substr(x + 1).c_str());
			}
		}
	}

	hfr::Deck deck;
	std::wstring err;
	if (!pdfRef.empty()) {
		return RenderPdfPage(pdfiumDll, pdfRef, slide, W, H, bmpOut) ? 0 : 1;
	}
	if (!hfr::LoadDeck(path, deck, err)) {
		printf("FAILED: %s\n", ToUtf8(err).c_str());
		return 1;
	}
	if (bmpOut.empty()) {
		printf("OK\n%s", hfr::DeckSummary(deck).c_str());
	if (!deck.debugInfo.empty()) {
		printf("--- debug ---\n%s", ToUtf8(deck.debugInfo).c_str());
	}
		return 0;
	}

	hfr::RenderSlide rs;
	if (!hfr::BuildSlide(deck, slide, W, H, rs, err)) {
		printf("RENDER FAILED: %s\n", ToUtf8(err).c_str());
		return 1;
	}
	std::vector<uint8_t> bgra;
	hfr::CompositeToBgra(rs, bgra);
	if (!hfr::WriteBmp32(bmpOut, W, H, bgra)) {
		printf("BMP WRITE FAILED\n");
		return 1;
	}
	int nText = 0, nRect = 0, nPic = 0, nTbl = 0;
	for (const hfr::Item &it : rs.items) {
		switch (it.kind) {
		case hfr::ItemKind::Text: nText++; break;
		case hfr::ItemKind::Rect: nRect++; break;
		case hfr::ItemKind::Picture: nPic++; break;
		case hfr::ItemKind::Table: nTbl++; break;
		}
	}
	printf("RENDER OK slide=%d size=%dx%d items=%zu (text=%d rect=%d pic=%d table=%d) -> %s\n", slide + 1, W, H,
	       rs.items.size(), nText, nRect, nPic, nTbl, ToUtf8(bmpOut).c_str());
	return 0;
}