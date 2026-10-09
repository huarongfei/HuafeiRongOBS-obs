#pragma once

#include <QWidget>

#include <obs.hpp>

class QLabel;
class QTextEdit;
class QPushButton;
class QCheckBox;
class HFRPptDocument;

/* 讲者视图窗口：
 *   - 左：当前页大图；右：备注 + 下一页预览 + 页码
 *   - 按钮与键盘（PageUp/PageDown/空格/左右键）翻页；可选"全局翻页"接管翻页笔
 *   - 与画布上的 PPT 来源共享同一文档实例，翻页后来源会重绘，投影/推流同步更新
 */
class HfrPresenterWindow : public QWidget {
public:
	explicit HfrPresenterWindow(QWidget *parent = nullptr);
	~HfrPresenterWindow() override;

	/* 绑定到某个 PPT 来源（内部文档）；source 可为空（则只操作控制台自检文档） */
	void BindSource(obs_source_t *source);
	void RefreshAll();

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;
	void closeEvent(QCloseEvent *event) override;

private:
	HFRPptDocument *Doc() const;
	void GotoNext();
	void GotoPrev();
	void UpdateImages();
	void UpdateNotes();

	obs_source_t *src = nullptr;   /* 已加引用 */
	QLabel *curView = nullptr;
	QLabel *nextView = nullptr;
	QLabel *pageLabel = nullptr;
	QTextEdit *notesView = nullptr;
	QPushButton *btnPrev = nullptr;
	QPushButton *btnNext = nullptr;
	QCheckBox *globalKeys = nullptr;
};

/* 打开/前置讲者视图（全局单例窗口） */
void HfrOpenPresenterWindow();