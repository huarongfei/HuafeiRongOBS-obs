#pragma once

#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>
#include <vector>

#include <obs.hpp>
#include <obs-frontend-api.h>

#include "OBSQTDisplay.hpp"

class QLabel;
class QSpinBox;
class QCheckBox;
class QComboBox;
class QListWidget;
class QPushButton;
class QTimer;

/* 独立 PPT 播放窗：全屏渲染自研引擎幻灯片（内部自建 slides 来源），
 * 不依赖导播坞/画布，适合"只用 OBS 播 PPT"的场合。
 * 键盘：←/PageUp 上一页、→/PageDown/空格 下一步(动画→翻页)、Home/End、B 黑屏、Esc 退出。 */
class HFRPptPlayerWindow : public OBSQTDisplay {
public:
	explicit HFRPptPlayerWindow(QWidget *parent = nullptr);
	~HFRPptPlayerWindow() override;

	bool OpenFile(const QString &path, QString *err = nullptr);
	void ShowOnScreen(int screenIndex);   /* 全屏显示在指定编号屏 */
	void ClosePlayer();

	void Next();       /* 先推进动画，没有则翻下一页 */
	void Prev();       /* 先回退动画，没有则翻上一页 */
	void NextAnim();
	void PrevAnim();
	void FirstPage();
	void LastPage();
	void SetPage(int page);
	void SetBlack(bool on);
	bool Black() const { return black; }
	/* 备注浮层：在播放窗底部叠加当前页备注（N 键切换） */
	void SetShowNotes(bool on);
	bool ShowNotes() const { return showNotes; }

	/* 播放到最后一步/最后一页仍按"下一步"时回调（连播队列用） */
	void SetEndReachedCallback(std::function<void()> cb) { onEndReached = std::move(cb); }
	bool AtEnd() const;

	obs_source_t *Source() const { return src; }
	QString FilePath() const { return filePath; }
	int Page() const;
	int PageCount() const;
	int AnimStep() const;
	int AnimStepCount() const;

protected:
	void keyPressEvent(QKeyEvent *event) override;
	void closeEvent(QCloseEvent *event) override;

private:
	static void Draw(void *data, uint32_t cx, uint32_t cy);
	void CreateInternalSource();

	obs_source_t *src = nullptr; /* 自建 slides 来源（持有引用） */
	QString filePath;
	bool black = false;
	bool fullscreen = true;
	std::function<void()> onEndReached;

	void UpdateNotesSprite();          /* UI 线程：把备注排版成 BGRA */
	void DrawNotesOverlay(uint32_t cx, uint32_t cy); /* 图形线程：上传并绘制 */
	class QTimer *notesTimer = nullptr;
	bool showNotes = false;
	QString notesCache;
	std::vector<uint8_t> notesPix;
	int notesPixW = 0, notesPixH = 0;
	bool notesDirty = false;
	struct gs_texture *notesTex = nullptr;
};

/* PVW 预览窗：独立全屏显示"下一页"（CPU 渲染快照 → 纹理），供操作员预览下一步 */
class HFRPptPreviewWindow : public OBSQTDisplay {
public:
	explicit HFRPptPreviewWindow(QWidget *parent = nullptr);
	~HFRPptPreviewWindow() override;

	void ShowOnScreen(int screenIndex);
	void ClosePreview();
	/* UI 线程调用：设置要显示的 BGRA 图（拷贝一份，图形线程上传） */
	void SetImage(const std::vector<uint8_t> &bgra, int w, int h);
	bool Visible() const { return isVisible(); }

protected:
	void closeEvent(QCloseEvent *event) override;

private:
	static void Draw(void *data, uint32_t cx, uint32_t cy);

	std::vector<uint8_t> pix;
	int pw = 0, ph = 0;
	bool dirty = false;
	struct gs_texture *tex = nullptr;
};

/* 独立 PPT 播放控制面板（置顶小窗）：打开文件 / 翻页 / 自动播放 / 黑屏 / 选屏 */
class HFRPptPlayerControl : public QWidget {
public:
	explicit HFRPptPlayerControl(QWidget *parent = nullptr);
	~HFRPptPlayerControl() override;

	void SetPlayer(HFRPptPlayerWindow *w);
	HFRPptPlayerWindow *Player() const { return player; }

private:
	void RefreshUi();

	HFRPptPlayerWindow *player = nullptr;
	QLabel *fileLabel = nullptr;
	QLabel *pageLabel = nullptr;
	QPushButton *btnOpen = nullptr;
	QPushButton *btnPrev = nullptr;
	QPushButton *btnNext = nullptr;
	QPushButton *btnNextAnim = nullptr;
	QPushButton *btnPrevAnim = nullptr;
	QPushButton *btnBlack = nullptr;
	QPushButton *btnPresenter = nullptr;
	QPushButton *btnThumbs = nullptr;
	QPushButton *btnNotes = nullptr;
	/* PVW 预览 */
	HFRPptPreviewWindow *preview = nullptr;
	QPushButton *btnPvw = nullptr;
	QComboBox *previewScreenCombo = nullptr;
	int lastPreviewPage = -2;
	QString lastPreviewFile;
	/* 多文件播放队列（连续播放） */
	QListWidget *queueList = nullptr;
	QPushButton *btnQueueAdd = nullptr;
	QPushButton *btnQueueDel = nullptr;
	QPushButton *btnQueueUp = nullptr;
	QPushButton *btnQueueDown = nullptr;
	QCheckBox *chkQueueAuto = nullptr;
	QCheckBox *chkQueueLoop = nullptr;
	QLabel *queueLabel = nullptr;
	QVector<QString> queueFiles;
	int queueIndex = -1;
	void PlayQueueFile(int index);
	void UpdateQueueLabel();
	void AdvanceQueue();
	QSpinBox *pageJump = nullptr;
	QPushButton *btnJump = nullptr;
	QLabel *timerLabel = nullptr;
	qint64 startMs = 0;
	QPushButton *btnShow = nullptr;
	QComboBox *screenCombo = nullptr;
	QCheckBox *autoPlay = nullptr;
	QSpinBox *autoSec = nullptr;
	QTimer *autoTimer = nullptr;
	QTimer *uiTimer = nullptr;
};

/* 打开/前置独立播放 UI（全局单例） */
void HfrOpenPptPlayerUi();
/* 退出前销毁独立播放 UI（在 obs_shutdown 之前调用，释放内部来源/纹理） */
void HfrShutdownPptPlayerUi();
