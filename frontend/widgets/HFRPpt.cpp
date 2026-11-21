#include "HFRPpt.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QUrl>

#include <obs-module.h>
#include <obs-frontend-api.h>

#ifdef HFR_ENABLE_PPT
#include <windows.h>

/* ---------------- pdfium 动态绑定（只用 C API，稳定） ---------------- */
namespace {

typedef void *FPDF_DOCUMENT;
typedef void *FPDF_PAGE;
typedef void *FPDF_BITMAP;

constexpr int FPDFBitmap_BGRA = 4;
constexpr int FPDF_ANNOT = 0x01;

struct PdfiumApi {
	HMODULE lib = nullptr;
	void (*InitLibrary)() = nullptr;
	void (*DestroyLibrary)() = nullptr;
	FPDF_DOCUMENT (*LoadMemDocument64)(const void *buf, size_t size, const char *password) = nullptr;
	void (*CloseDocument)(FPDF_DOCUMENT doc) = nullptr;
	int (*GetPageCount)(FPDF_DOCUMENT doc) = nullptr;
	FPDF_PAGE (*LoadPage)(FPDF_DOCUMENT doc, int index) = nullptr;
	void (*ClosePage)(FPDF_PAGE page) = nullptr;
	float (*GetPageWidthF)(FPDF_PAGE page) = nullptr;
	float (*GetPageHeightF)(FPDF_PAGE page) = nullptr;
	FPDF_BITMAP (*Bitmap_CreateEx)(int w, int h, int format, void *first_scan, int stride) = nullptr;
	void (*Bitmap_FillRect)(FPDF_BITMAP bmp, int left, int top, int w, int h, unsigned long color) = nullptr;
	void (*Bitmap_Destroy)(FPDF_BITMAP bmp) = nullptr;
	void (*RenderPageBitmap)(FPDF_BITMAP bmp, FPDF_PAGE page, int x, int y, int w, int h, int rotate, int flags) = nullptr;
	unsigned long (*GetLastError)() = nullptr;
};

PdfiumApi g_pdf;

template <typename T> void Bind(T &fn, const char *name)
{
	fn = reinterpret_cast<T>(GetProcAddress(g_pdf.lib, name));
}

bool LoadPdfium(const QString &loRoot)
{
	if (g_pdf.lib) {
		return true;
	}
	const QString dll = QDir::toNativeSeparators(loRoot + QStringLiteral("/program/pdfiumlo.dll"));
	const std::wstring w = dll.toStdWString();
	/* pdfiumlo.dll 自身依赖 lo\program 下的库：必须用"替代搜索路径"加载，
	 * 否则会 126（找不到模块）——已在隔离测试中复现并验证修复。 */
	g_pdf.lib = LoadLibraryExW(w.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!g_pdf.lib) {
		g_pdf.lib = LoadLibraryW(w.c_str());
	}
	if (!g_pdf.lib) {
		blog(LOG_WARNING, "[HFR-PPT] 加载 pdfiumlo.dll 失败（%lu）：%s", (unsigned long)GetLastError(),
		     dll.toUtf8().constData());
		return false;
	}
	Bind(g_pdf.InitLibrary, "FPDF_InitLibrary");
	Bind(g_pdf.DestroyLibrary, "FPDF_DestroyLibrary");
	Bind(g_pdf.LoadMemDocument64, "FPDF_LoadMemDocument64");
	Bind(g_pdf.CloseDocument, "FPDF_CloseDocument");
	Bind(g_pdf.GetPageCount, "FPDF_GetPageCount");
	Bind(g_pdf.LoadPage, "FPDF_LoadPage");
	Bind(g_pdf.ClosePage, "FPDF_ClosePage");
	Bind(g_pdf.GetPageWidthF, "FPDF_GetPageWidthF");
	Bind(g_pdf.GetPageHeightF, "FPDF_GetPageHeightF");
	Bind(g_pdf.Bitmap_CreateEx, "FPDFBitmap_CreateEx");
	Bind(g_pdf.Bitmap_FillRect, "FPDFBitmap_FillRect");
	Bind(g_pdf.Bitmap_Destroy, "FPDFBitmap_Destroy");
	Bind(g_pdf.RenderPageBitmap, "FPDF_RenderPageBitmap");

	if (!g_pdf.InitLibrary || !g_pdf.LoadMemDocument64 || !g_pdf.GetPageCount || !g_pdf.LoadPage ||
	    !g_pdf.Bitmap_CreateEx || !g_pdf.RenderPageBitmap) {
		blog(LOG_WARNING, "[HFR-PPT] pdfium 缺少必要导出");
		FreeLibrary(g_pdf.lib);
		g_pdf.lib = nullptr;
		return false;
	}
	g_pdf.InitLibrary();
	blog(LOG_INFO, "[HFR-PPT] pdfium 已初始化（%s）", dll.toUtf8().constData());
	return true;
}

} // namespace

#else

namespace {
bool LoadPdfium(const QString &)
{
	return false;
}
} // namespace

#endif /* HFR_ENABLE_PPT */

/* =============================== HFRPpt =============================== */

HFRPpt &HFRPpt::Instance()
{
	static HFRPpt inst;
	return inst;
}

HFRPpt::~HFRPpt()
{
#ifdef HFR_ENABLE_PPT
	if (g_pdf.lib) {
		if (g_pdf.DestroyLibrary) {
			g_pdf.DestroyLibrary();
		}
		FreeLibrary(g_pdf.lib);
		g_pdf.lib = nullptr;
	}
#endif
}

QString HFRPpt::DefaultLoPath()
{
	/* 1) 嵌入式：obs64.exe 同目录 lo\ */
	const QString embedded = QCoreApplication::applicationDirPath() + QStringLiteral("/lo");
	if (QFile::exists(embedded + QStringLiteral("/program/pdfiumlo.dll"))) {
		return embedded;
	}
	/* 2) 环境变量 */
	const QByteArray env = qgetenv("HFR_LO_PATH");
	if (!env.isEmpty()) {
		const QString p = QString::fromLocal8Bit(env);
		if (QFile::exists(p + QStringLiteral("/program/pdfiumlo.dll"))) {
			return p;
		}
	}
	/* 3) 开发目录 */
	return QStringLiteral("D:/HuafeirongOBS/tools/LibreOffice");
}

bool HFRPpt::Enabled() const
{
#ifdef HFR_ENABLE_PPT
	return true;
#else
	return false;
#endif
}

bool HFRPpt::IsLoaded() const
{
	return pdfium != nullptr;
}

bool HFRPpt::EnsureLoaded(QString *errOut)
{
	if (pdfium) {
		return true;
	}
	const QString loRoot = DefaultLoPath();
#ifdef HFR_ENABLE_PPT
	if (!LoadPdfium(loRoot)) {
		lastError = QStringLiteral("无法加载 pdfium（%1/program/pdfiumlo.dll）。请确认程序目录包含完整 lo\\ 运行时，"
					   "或运行 scripts/fetch-libreoffice-runtime.cmd。")
				    .arg(loRoot);
		if (errOut) {
			*errOut = lastError;
		}
		return false;
	}
	const QString soffice = loRoot + QStringLiteral("/program/soffice.exe");
	if (!QFile::exists(soffice)) {
		lastError = QStringLiteral("找不到 soffice.exe：%1").arg(soffice);
		if (errOut) {
			*errOut = lastError;
		}
		return false;
	}
	pdfium = g_pdf.lib;
	blog(LOG_INFO, "[HFR-PPT] 运行时就绪：LO=%s，版本=%s", loRoot.toUtf8().constData(),
	     HfrLibreOfficeVersion().toUtf8().constData());
	return true;
#else
	lastError = QStringLiteral("本构建未启用 PPT（-DHFR_ENABLE_PPT=ON）");
	if (errOut) {
		*errOut = lastError;
	}
	return false;
#endif
}

QString HfrLibreOfficeVersion()
{
	const QString loRoot = HFRPpt::DefaultLoPath();
	QFile f(loRoot + QStringLiteral("/program/version.ini"));
	if (!f.open(QIODevice::ReadOnly)) {
		return QString();
	}
	const QString text = QString::fromUtf8(f.readAll());
	f.close();
	const int k = text.indexOf(QStringLiteral("buildid="));
	if (k < 0) {
		return QString();
	}
	int end = text.indexOf(QLatin1Char('\n'), k);
	if (end < 0) {
		end = text.size();
	}
	return text.mid(k + 8, end - k - 8).trimmed();
}

QString HfrPptTestOutputDir()
{
	char *p = obs_frontend_get_current_profile_path();
	QString dir = p ? QString::fromUtf8(p) : QString();
	if (p) {
		bfree(p);
	}
	if (dir.isEmpty()) {
		dir = QDir::tempPath();
	}
	QDir().mkpath(dir);
	return dir;
}

QString HfrPptCacheDir()
{
	const QString base = HfrPptTestOutputDir();
	const QString dir = base + QStringLiteral("/hfr_ppt_cache");
	QDir().mkpath(dir);
	return dir;
}

bool HFRPpt::ConvertToPdf(const QString &srcFile, QString *pdfOut, QString *errOut)
{
#ifdef HFR_ENABLE_PPT
	QString err;
	if (!EnsureLoaded(&err)) {
		if (errOut) {
			*errOut = err;
		}
		return false;
	}
	if (!QFile::exists(srcFile)) {
		if (errOut) {
			*errOut = QStringLiteral("文件不存在：%1").arg(srcFile);
		}
		return false;
	}

	const QFileInfo fi(srcFile);
	const QString cacheDir = HfrPptCacheDir();
	const QString pdfPath = cacheDir + QLatin1Char('/') + fi.completeBaseName() + QStringLiteral(".pdf");

	/* 缓存：PDF 比源文件新则直接用 */
	if (QFile::exists(pdfPath) && QFileInfo(pdfPath).lastModified() >= fi.lastModified()) {
		if (pdfOut) {
			*pdfOut = pdfPath;
		}
		return true;
	}

	const QString loRoot = DefaultLoPath();
	const QString soffice = loRoot + QStringLiteral("/program/soffice.exe");
	QProcess proc;
	proc.setWorkingDirectory(loRoot);
	QStringList args;
	args << QStringLiteral("--headless") << QStringLiteral("--norestore") << QStringLiteral("--nolockcheck")
	     << QStringLiteral("--convert-to") << QStringLiteral("pdf") << QStringLiteral("--outdir") << cacheDir
	     << QDir::toNativeSeparators(srcFile);
	blog(LOG_INFO, "[HFR-PPT] 转换中：soffice --convert-to pdf（一次性，非桥接）");
	proc.start(soffice, args);
	if (!proc.waitForStarted(15000)) {
		if (errOut) {
			*errOut = QStringLiteral("无法启动 soffice.exe");
		}
		return false;
	}
	if (!proc.waitForFinished(300000)) {
		proc.kill();
		proc.waitForFinished(5000);
		if (errOut) {
			*errOut = QStringLiteral("soffice 转换超时");
		}
		return false;
	}
	if (!QFile::exists(pdfPath)) {
		const QString out = QString::fromLocal8Bit(proc.readAllStandardOutput() + proc.readAllStandardError());
		if (errOut) {
			*errOut = QStringLiteral("转换未生成 PDF（soffice 输出：%1）").arg(out.trimmed().left(300));
		}
		return false;
	}
	blog(LOG_INFO, "[HFR-PPT] 转换完成：%s", pdfPath.toUtf8().constData());
	if (pdfOut) {
		*pdfOut = pdfPath;
	}
	return true;
#else
	Q_UNUSED(srcFile);
	Q_UNUSED(pdfOut);
	if (errOut) {
		*errOut = QStringLiteral("本构建未启用 PPT（-DHFR_ENABLE_PPT=ON）");
	}
	return false;
#endif
}

/* ========================== HFRPptDocument ========================== */

HFRPptDocument::~HFRPptDocument()
{
	Close();
}

bool HFRPptDocument::Open(const QString &pptxPath, QString *errOut)
{
#ifdef HFR_ENABLE_PPT
	Close();
	QString pdf;
	if (!HFRPpt::Instance().ConvertToPdf(pptxPath, &pdf, errOut)) {
		return false;
	}
	QFile f(pdf);
	if (!f.open(QIODevice::ReadOnly)) {
		if (errOut) {
			*errOut = QStringLiteral("无法读取 PDF：%1").arg(pdf);
		}
		return false;
	}
	const QByteArray data = f.readAll();
	f.close();

	FPDF_DOCUMENT d = g_pdf.LoadMemDocument64(data.constData(), (size_t)data.size(), nullptr);
	if (!d) {
		if (errOut) {
			*errOut = QStringLiteral("pdfium 打开 PDF 失败（err=%1）")
					  .arg(g_pdf.GetLastError ? (unsigned long)g_pdf.GetLastError() : 0UL);
		}
		return false;
	}
	doc = d;
	pageCount = g_pdf.GetPageCount(d);
	current = 0;
	pdfPath = pdf;
	blog(LOG_INFO, "[HFR-PPT] 文档已载入：%s（%d 页）", QFileInfo(pptxPath).fileName().toUtf8().constData(), pageCount);
	return true;
#else
	Q_UNUSED(pptxPath);
	if (errOut) {
		*errOut = QStringLiteral("本构建未启用 PPT（-DHFR_ENABLE_PPT=ON）");
	}
	return false;
#endif
}

void HFRPptDocument::Close()
{
#ifdef HFR_ENABLE_PPT
	if (doc) {
		g_pdf.CloseDocument((FPDF_DOCUMENT)doc);
		doc = nullptr;
	}
#endif
	pageCount = 0;
	current = 0;
	pdfPath.clear();
}

bool HFRPptDocument::SetPart(int index)
{
	if (!doc || index < 0 || index >= pageCount) {
		return false;
	}
	current = index;
	return true;
}

bool HFRPptDocument::NextPart()
{
	return SetPart(current + 1);
}

bool HFRPptDocument::PrevPart()
{
	return SetPart(current - 1);
}

bool HFRPptDocument::RenderPart(int index, uint32_t w, uint32_t h, std::vector<uint8_t> &bgraOut, QString *errOut)
{
#ifdef HFR_ENABLE_PPT
	if (!doc) {
		if (errOut) {
			*errOut = QStringLiteral("尚未打开 PPT");
		}
		return false;
	}
	if (index < 0 || index >= pageCount || w == 0 || h == 0) {
		return false;
	}
	FPDF_PAGE page = g_pdf.LoadPage((FPDF_DOCUMENT)doc, index);
	if (!page) {
		if (errOut) {
			*errOut = QStringLiteral("加载第 %1 页失败").arg(index + 1);
		}
		return false;
	}
	bgraOut.assign((size_t)w * h * 4, 0xFF);
	FPDF_BITMAP bmp = g_pdf.Bitmap_CreateEx((int)w, (int)h, FPDFBitmap_BGRA, bgraOut.data(), (int)(w * 4));
	if (!bmp) {
		g_pdf.ClosePage(page);
		if (errOut) {
			*errOut = QStringLiteral("创建位图失败");
		}
		return false;
	}
	g_pdf.Bitmap_FillRect(bmp, 0, 0, (int)w, (int)h, 0xFFFFFFFFu); /* 白底 */

	/* 等比居中（幻灯片通常与目标同比例，边界情况留白） */
	const float pw = g_pdf.GetPageWidthF ? g_pdf.GetPageWidthF(page) : (float)w;
	const float ph = g_pdf.GetPageHeightF ? g_pdf.GetPageHeightF(page) : (float)h;
	int dw = (int)w, dh = (int)h, ox = 0, oy = 0;
	if (pw > 0 && ph > 0) {
		const double s = qMin((double)w / pw, (double)h / ph);
		dw = qMax(1, (int)(pw * s));
		dh = qMax(1, (int)(ph * s));
		ox = ((int)w - dw) / 2;
		oy = ((int)h - dh) / 2;
	}
	g_pdf.RenderPageBitmap(bmp, page, ox, oy, dw, dh, 0, FPDF_ANNOT);
	g_pdf.Bitmap_Destroy(bmp);
	g_pdf.ClosePage(page);
	return true;
#else
	Q_UNUSED(index);
	Q_UNUSED(w);
	Q_UNUSED(h);
	if (errOut) {
		*errOut = QStringLiteral("本构建未启用 PPT（-DHFR_ENABLE_PPT=ON）");
	}
	return false;
#endif
}

bool HFRPptDocument::RenderCurrentPart(uint32_t w, uint32_t h, std::vector<uint8_t> &bgraOut, QString *errOut)
{
	return RenderPart(current, w, h, bgraOut, errOut);
}

QString HFRPptDocument::NotesText() const
{
	/* 备注：后续从 pptx 的 notesSlide 解析（PDF 不含备注）。
	 * 现阶段返回空串，讲者视图会给出提示。 */
	return QString();
}