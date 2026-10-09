#pragma once

#include <QString>
#include <cstdint>
#include <vector>

/* ============================================================
 * HFRPpt —— 本地 LibreOffice 幻灯片渲染（无常驻桥接）
 *
 * 流水线：
 *   1) 导入时调用随包 LibreOffice 的 soffice.exe（--headless --convert-to pdf），
 *      一次性把 pptx 转成 PDF，转换结束进程即退出（**不是常驻桥**）；
 *   2) 进程内用 LibreOffice 自带的 pdfium（pdfiumlo.dll）逐页渲染为 BGRA 位图，
 *      供来源纹理 / 讲者视图使用。
 *
 * 为什么不用进程内 LibreOfficeKit(LOK)：官方 LOK 定位为 Linux，Windows 版为实验性；
 * 实测两种入口（lok_preinit_2 / libreofficekit_hook_2）在 stock 版 LO 上均于
 * hook 内部直接 AV（0xC0000005），无法在产品中依赖。
 *
 * 附带收益：PDF 内嵌字体 → 中文与缺字体环境的排版一致性显著好于直接渲染 pptx。
 * 动画：静态按"每页最终态"呈现（见 docs/PPT-方案.md 一期策略）。
 * ============================================================ */

bool HfrRegisterPptSource();          /* 注册 PPT 来源（来源→+） */
QString HfrLibreOfficeVersion();      /* 随包 LibreOffice 版本（读 program/version.ini） */
QString HfrPptTestOutputDir();        /* 自检/预览输出目录 */
QString HfrPptCacheDir();             /* 转出的 PDF 缓存目录 */

class HFRPptDocument {
public:
	HFRPptDocument() = default;
	~HFRPptDocument();
	HFRPptDocument(const HFRPptDocument &) = delete;
	HFRPptDocument &operator=(const HFRPptDocument &) = delete;

	/* 打开 pptx/ppt/odp（必要时先转 PDF，再加载） */
	bool Open(const QString &pptxPath, QString *errOut);
	void Close();
	bool IsOpen() const { return doc != nullptr; }

	int PartCount() const { return pageCount; }
	int CurrentPart() const { return current; }
	bool SetPart(int index);
	bool NextPart();
	bool PrevPart();

	/* 整页渲染为 BGRA（自顶向下，每像素 4 字节；按目标尺寸等比居中，白底） */
	bool RenderCurrentPart(uint32_t w, uint32_t h, std::vector<uint8_t> &bgraOut, QString *errOut);
	bool RenderPart(int index, uint32_t w, uint32_t h, std::vector<uint8_t> &bgraOut, QString *errOut);

	/* 备注文本（当前实现返回空串；后续从 pptx 备注页解析） */
	QString NotesText() const;

	QString PdfPath() const { return pdfPath; }

private:
	void *doc = nullptr;      /* FPDF_DOCUMENT */
	int pageCount = 0;
	int current = 0;
	QString pdfPath;
};

class HFRPpt {
public:
	static HFRPpt &Instance();

	static QString DefaultLoPath();      /* 嵌入式 lo\ 优先 → HFR_LO_PATH → 开发目录 */
	bool Enabled() const;
	bool EnsureLoaded(QString *errOut);  /* 加载 pdfium + 校验 soffice 可用 */
	bool IsLoaded() const;
	void *Kit() const { return pdfium; }

	/* 把源文件转成 PDF（带缓存：PDF 比源文件新则复用） */
	bool ConvertToPdf(const QString &srcFile, QString *pdfOut, QString *errOut);

	QString LastError() const { return lastError; }

private:
	HFRPpt() = default;
	~HFRPpt();
	HFRPpt(const HFRPpt &) = delete;
	HFRPpt &operator=(const HFRPpt &) = delete;

	QString lastError;
	void *lib = nullptr;      /* pdfiumlo.dll */
	void *pdfium = nullptr;   /* 标记：pdfium 已初始化 */
};
