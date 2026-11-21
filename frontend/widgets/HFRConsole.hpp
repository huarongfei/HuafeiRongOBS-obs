#pragma once

#include "HFRSpliceDialog.hpp"
#include "OBSQTDisplay.hpp"

#include <obs.hpp>

#include <QHash>
#include <QDockWidget>
#include <QPointer>
#include <QRect>
#include <QVector>

class QCheckBox;
class QComboBox;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QLabel;
class QScreen;
class QSpinBox;
class QTabWidget;
class QTimer;
class HFRSwitcherPanel;

/* ============================================================
 * HuafeiRongOBS 嵌入式多画布 UI（S2）
 *  - 画布行 = 实时缩略预览 + 名称/分辨率（点选行 = 选中该画布为操作对象）
 *  - 显示器自动编号；"投影到选中屏"全屏受管
 *  - "填充测试色块"就地给画布一个可见内容（新画布默认为空→黑屏的验证手段）
 * ============================================================ */

/* 画布行控件：缩略预览 + 文字 */
class HFRCanvasRow : public QWidget {
public:
	HFRCanvasRow(obs_canvas_t *cv, bool recording, QWidget *parent = nullptr);
	~HFRCanvasRow() override;

	/* 仅更新 REC 指示（避免为录制状态重建整行控件） */
	void SetRecording(bool recording);

private:
	static void DrawThumb(void *data, uint32_t cx, uint32_t cy);
	obs_canvas_t *canvas = nullptr;
	OBSQTDisplay *thumb = nullptr;
	QLabel *nameLabel = nullptr; /* REC 指示动态更新用 */
	bool ready = false;
	qint64 lastDrawNs = 0; /* 缩略图节流：约 10fps */
};

/* 画布投影窗：画布 → 指定编号显示器（全屏、受管） */
class HFRCanvasProjector : public OBSQTDisplay {
public:
	/* tileRect 非空 = 只把画布上该像素矩形铺满投影窗（异形拼接的一块）；
	 * 空 = 整幅画布等比缩放居中（letterbox）。 */
	HFRCanvasProjector(obs_canvas_t *canvas, QScreen *screen, QWidget *parent = nullptr,
			   QRect tileRect = QRect(), int tileRot = 0, int blendL = 0, int blendR = 0,
			   int blendT = 0, int blendB = 0, const HfrWallTile *keystone = nullptr);
	~HFRCanvasProjector() override;

	void SetSafeArea(bool on) { safeArea = on; }
	bool HasSafeArea() const { return safeArea; }

private:
	static void Draw(void *data, uint32_t cx, uint32_t cy);
	static bool DrawKeystoneQuad(gs_texture_t *tex, HFRCanvasProjector *w, uint32_t cx, uint32_t cy);
	static bool DrawCurveMesh(gs_texture_t *tex, HFRCanvasProjector *w, uint32_t cx, uint32_t cy);
	static void DrawBlendSafeOverlays(HFRCanvasProjector *w, uint32_t cx, uint32_t cy);
	obs_canvas_t *canvas = nullptr;
	bool ready = false;
	bool safeArea = false;
	QRect tile;
	bool hasTile = false;
	int tileRot = 0; /* 0/90/180/270，仅拼接块投影有效 */
	int blendL = 0, blendR = 0, blendT = 0, blendB = 0; /* 边缘融合宽（墙像素） */
	/* 四角几何校正 */
	int kTLx = 0, kTLy = 0, kTRx = 0, kTRy = 0, kBLx = 0, kBLy = 0, kBRx = 0, kBRy = 0;
	bool hasKeystone = false;
	/* 色彩统一 */
	double gammaV = 1.0;
	double liftV = 0.0;
	double gainV = 1.0;
	bool hasColorAdj = false;
	/* 曲面网格（3x3 控制点中的 5 个非角点） */
	int cMTx = 0, cMTy = 0, cMRx = 0, cMRy = 0, cMBx = 0, cMBy = 0, cMLx = 0, cMLy = 0;
	int cCx = 0, cCy = 0;
	bool hasCurve = false;
	struct gs_vertex_buffer *curveVb = nullptr;
	uint32_t curveVbW = 0, curveVbH = 0;
};

/* 画布级动态热键数据（回调上下文；每次刷新画布列表重建） */
struct HfrCanvasHotkeyData {
	class HFRConsoleDock *dock = nullptr;
	int canvasRow = -1; /* 0=主画布，1..N=用户画布（与画布列表行一致） */
	int kind = 0;       /* 0=投影到屏，1=切场景 */
	int arg = 0;        /* 屏幕编号 / 场景序号(1起) */
};

/* 投影记录：画布 → 屏编号（矩阵内受管，可单独停止） */
struct HFRProjection {
	QPointer<HFRCanvasProjector> window;
	obs_canvas_t *canvas = nullptr;
	int screenIndex = -1;
	QRect tile;      /* 非空 = 拼接分块投影 */
	bool hasTile = false;
	int tileRot = 0;
	int blendL = 0, blendR = 0, blendT = 0, blendB = 0;
	int kTLx = 0, kTLy = 0, kTRx = 0, kTRy = 0, kBLx = 0, kBLy = 0, kBRx = 0, kBRy = 0;
	bool hasKeystone = false;
	double gammaV = 1.0;
	double liftV = 0.0;
	double gainV = 1.0;
	bool hasColorAdj = false;
	int cMTx = 0, cMTy = 0, cMRx = 0, cMRy = 0, cMBx = 0, cMBy = 0, cMLx = 0, cMLy = 0;
	int cCx = 0, cCy = 0;
	bool hasCurve = false;
};

/* 命名布局：一组（画布 → 屏编号）映射，可一键保存/还原 */
struct HfrLayoutItem {
	QString canvas;
	int screen = -1;
	bool safe = false;
};

struct HfrLayout {
	QString name;
	QVector<HfrLayoutItem> items;
};

/* 输出矩阵 / 导播坞 */
class HFRConsoleDock : public QDockWidget {
public:
	explicit HFRConsoleDock(QWidget *parent = nullptr);
	~HFRConsoleDock() override;

	static HFRConsoleDock *Get() { return s_instance; }

	void RefreshMonitors();
	void RefreshCanvasList();
	/* 仅刷新各行 REC 指示（避免录制开始/停止时重建行控件与 OBSQTDisplay） */
	void UpdateRecordingIndicators();
	/* 一键应用默认前端热键（Ctrl+Alt+1..9） */
	void ApplyDefaultHotkeys();
	/* 导出诊断信息（版本/屏幕/画布/拼接/投影/录制/节目单/源/日志尾部） */
	void ExportDiagnostics();
	void CreateCanvas();
	void RemoveSelectedCanvas();
	void RenameSelectedCanvas();
	void ProjectCanvasToMonitor(obs_canvas_t *cv, int mRow);
	void ProjectSelected();
	/* 一键黑场：所有画布通道切到黑场场景；再点恢复原场景 */
	void ToggleBlackout();

	/* 异形屏拼接：打开分块编辑窗，应用后逐块投影 */
	void OpenSpliceDialog();
	void ApplySplice(obs_canvas_t *cv, const QVector<HfrWallTile> &tiles);
	void ProjectTileToMonitor(obs_canvas_t *cv, const HfrWallTile &t);
	void StopProjectionOfSelected();
	void CloseAllProjectors();
	/* 多屏布局：把当前投影编排存成命名布局，一键还原 */
	void SaveCurrentLayout();
	void ApplySelectedLayout();
	void DeleteSelectedLayout();
	void RefreshLayoutCombo();
	void CleanupForShutdown();
	/* 把演出动作注册为 OBS 前端热键（可在 设置→热键 绑定，也可由 obs-websocket 远程 TriggerHotkeyByName 触发） */
	void RegisterHfrHotkeys();
	/* 画布级动态热键：hfr.canvas.<n>.project_screen.<m> / hfr.canvas.<n>.scene.<k>
	 * 由 obs-websocket TriggerHotkeyByName 远程触发（n/m/k 见 docs/远程控制热键.md） */
	void RegisterCanvasHotkeys();
	void UnregisterCanvasHotkeys();
	/* 热键动作：执行当前选中 cue / 当前画布投主屏 */
	void ExecuteCurrentCue();
	void ProjectSelectedToPrimary();
	/* 画布级热键动作（供动态热键回调调用） */
	void CanvasActivateProject(int canvasRow, int screenIndex);
	void CanvasActivateScene(int canvasRow, int sceneOrdinal); /* 退出前：关投影并释放全部用户画布（obs 仍存活时） */
	void StartRecordingSelected();     /* 把选中画布录制到独立文件（编码器绑画布 video） */
	void StopRecordingSelected();      /* 停止选中画布的录制 */
	void StopAllRecordings();
	/* 音频双轨：解说源设置/恢复/刷新 */
	void RefreshCommentatorCombo();
	void ApplyCommentatorSource();
	void OpenAudioRouting(); /* 音频路由矩阵（每源选择录制音轨） */
	void RestoreCommentatorMixers();          /* 停止全部画布录制（退出/清理时用） */
	void PptOpenTest();               /* PPT：选择 pptx，用 LOK 渲染验证（BMP + 统计） */
	void PptNext();                    /* PPT：下一页 */
	void PptPrev();                    /* PPT：上一页 */
	void ToggleSafeAreaOnSelected(); /* 选中画布的投影：安全区网格开关 */
	void UpdateSceneListForSelection(); /* 场景作用域：列出当前画布的场景 */
	void CreateSceneForSelected();      /* 为当前画布新建场景 */
	void SwitchToSelectedScene();       /* 把当前画布切到选中场景 */
	void DeleteSelectedScene();         /* 删除当前画布的选中场景 */
	void FillColorIntoSelectedScene();  /* 给选中场景加一块铺满的色块（演示内容） */

	/* ---------------- 节目单编排（Rundown） ----------------
	 * 一条节目 = (画布, 场景)；按顺序执行即切换对应画布的节目场景。
	 * 支持手动上/下一个与定时间隔自动播放，随状态文件持久化。 */
	void AddCurrentSceneToRundown(); /* 把当前选中画布的场景加入节目单 */
	void RemoveSelectedCue();        /* 删除节目单选中项 */
	void MoveCue(int delta);         /* 节目单条目上移/下移 */
	void ExecuteCue(int index);      /* 执行第 index 条（按类型：场景/幻灯片/黑场） */
	void AddSpecialCue(int type);    /* 追加特殊条目（幻灯片翻页/黑场/恢复） */
	void AddDelayCue(int seconds);   /* 追加延时条目 */
	void AddScopeCue();              /* 追加"切换操作画布"条目（当前选中画布） */
	void NextCue();                  /* 执行下一条 */
	void PrevCue();                  /* 执行上一条 */
	void ToggleAutoPlay();           /* 开始/停止定时自动播放 */
	void RefreshCueList();           /* 刷新节目单列表显示 */
	void ExportRundown();            /* 导出节目单为 JSON 文件 */
	void ImportRundown();            /* 从 JSON 导入并（确认后）替换节目单 */

	/* ---------------- 素材池 / 倒计时（计划单补齐） ---------------- */
	void ImportMediaIntoScene(); /* 选择图片/视频/音频，加入当前画布的场景（视频默认循环） */
	void ImportMediaFile(const QString &file); /* 把单个媒体文件加入当前场景（拖拽复用） */
	void AddCountdownOverlay();  /* 在当前场景加倒计时文本（先输入分钟数 1–600） */
	void RefreshMediaList();     /* 刷新本会话导入的素材列表 */
	QString StateFilePath() const;   /* profile 目录下的状态文件路径 */
	void SaveState();                /* 持久化画布/投影布局到 profile 目录 JSON */
	void LoadState();                /* 启动时恢复 */
	void SetTestColorOnSelected(); /* 给选中画布就地填一块测试色 */
	void SetCanvasTestColor(obs_canvas_t *cv);

protected:
	/* 拖拽素材文件到导播坞 → 导入当前场景 */
	void dragEnterEvent(QDragEnterEvent *event) override;
	void dropEvent(QDropEvent *event) override;

private:
	obs_canvas_t *CanvasFromRow(int row) const;
	void AddCanvasRowItem(obs_canvas_t *cv, const QString &label);

	/* UI 构建与信号连接单独维护（HFRConsoleUi.cpp），业务逻辑见 HFRConsole.cpp */
	void BuildUi();
	void WireSignals();
	/* 状态栏级别：0=普通 1=警告 2=错误 */
	void SetStatus(const QString &text, int level = 0);
	/* 素材列表双击定位：切到该素材所在画布/场景并提示源名 */
	void LocateMediaAt(int row);

	QTabWidget *tabs = nullptr;
	QListWidget *monitorList = nullptr;
	QListWidget *canvasList = nullptr;
	QListWidget *sceneList = nullptr;
	QPushButton *btnRefresh = nullptr;
	QPushButton *btnNew = nullptr;
	QPushButton *btnRemove = nullptr;
	QPushButton *btnRename = nullptr;
	QPushButton *btnTest = nullptr;
	QPushButton *btnProject = nullptr;
	struct HfrRecording {
		obs_output_t *out = nullptr;
		obs_encoder_t *venc = nullptr;
		QVector<obs_encoder_t *> aencs; /* 按音频路由矩阵创建的多轨编码器（索引=音轨） */
		QString path;
	};
	QHash<obs_canvas_t *, HfrRecording> recordings; /* 每画布一路录制 */


	QPushButton *btnPptOpen = nullptr;
	QPushButton *btnPptPrev = nullptr;
	QPushButton *btnPptNext = nullptr;
	QPushButton *btnPresenter = nullptr;
	QPushButton *btnPptPlayer = nullptr; /* 独立 PPT 播放 UI */
	QPushButton *btnFontFix = nullptr;  /* 缺字体一键替换向导 */
	QComboBox *langCombo = nullptr;    /* 界面语言（重启生效） */
	QPushButton *btnDiag = nullptr;   /* 导出诊断信息（排障） */
	QPushButton *btnHotkeyHelp = nullptr; /* 快捷键帮助 */
	QPushButton *btnDefaultHotkeys = nullptr; /* 应用默认热键 */
	/* 前端热键 id（用于应用默认绑定） */
	obs_hotkey_id hkCueNext = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id hkCuePrev = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id hkCueRun = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id hkCueAuto = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id hkBlackout = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id hkProjectPrimary = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id hkStopProjections = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id hkRecStart = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id hkRecStop = OBS_INVALID_HOTKEY_ID;

	QVector<obs_hotkey_id> canvasHotkeyIds;
	QVector<struct HfrCanvasHotkeyData *> canvasHotkeyData;
	QPushButton *btnSlidesOpen = nullptr;
	QPushButton *btnSlidesPrev = nullptr;
	QPushButton *btnSlidesNext = nullptr;
	QPushButton *btnAnimNext = nullptr;
	QPushButton *btnAnimPrev = nullptr;
	QComboBox *layoutCombo = nullptr;
	QPushButton *btnLayoutSave = nullptr;
	QPushButton *btnLayoutApply = nullptr;
	QPushButton *btnLayoutDel = nullptr;
	QVector<HfrLayout> layouts;
	/* 音频双轨录制：第 2 轨只收"解说源"（该源 mixers 置为 2，只进第2轨） */
	QCheckBox *chkDualTrack = nullptr;
	QComboBox *commentatorCombo = nullptr;
	QPushButton *btnCommentatorApply = nullptr;
	QPushButton *btnAudioRoute = nullptr;
	QHash<obs_source_t *, uint32_t> commentatorPrev; /* 应用解说源前的 mixers（持有引用） */
	/* 每画布的拼接块定义（异形屏）：画布分辨率即墙分辨率 */
	QHash<obs_canvas_t *, QVector<HfrWallTile>> wallTiles;
	QPushButton *btnSplice = nullptr;
	QPushButton *btnBlackout = nullptr;
	bool blackoutActive = false;
	QHash<obs_canvas_t *, obs_source_t *> blackoutPrev; /* 黑场前的通道场景（持有引用） */

	QComboBox *encCombo = nullptr;
	QPushButton *btnRecStart = nullptr;
	QPushButton *btnRecStop = nullptr;
	QPushButton *btnRecStopAll = nullptr;
	/* 节目单（Rundown） */
	struct HfrCue {
		int type = 0; /* 0=场景；1=幻灯片下一页；2=幻灯片上一页；3=黑场；4=恢复画面；5=延时；6=切换操作画布 */
		int waitMs = 0; /* type=5：延时毫秒 */
		QString canvas;
		QString scene;
	};
	QListWidget *cueList = nullptr;
	QListWidget *mediaList = nullptr;
	QPushButton *btnMediaAdd = nullptr;
	QPushButton *btnCountdown = nullptr;
	QTimer *countdownTimer = nullptr;
	QHash<obs_source_t *, qint64> countdowns; /* 倒计时文本源 → 结束时间(ms) */
	/* 素材记录：双击可定位回其所在画布/场景 */
	struct HfrMedia {
		QString file;   /* 文件名（不含路径） */
		QString type;   /* 图片 / 音视频 */
		QString canvas; /* 画布名 */
		QString scene;  /* 场景名 */
		QString source; /* 源名 */
	};
	QVector<HfrMedia> mediaFiles; /* 本会话导入的素材（显示/定位用） */

	QSpinBox *cueInterval = nullptr;
	QPushButton *btnCueAdd = nullptr;
	QPushButton *btnCueDel = nullptr;
	QPushButton *btnCueUp = nullptr;
	QPushButton *btnCueDown = nullptr;
	QPushButton *btnCueRun = nullptr;
	QPushButton *btnCuePrev = nullptr;
	QPushButton *btnCueNext = nullptr;
	QPushButton *btnCueAuto = nullptr;
	QPushButton *btnCueSpecialSlidesNext = nullptr;
	QPushButton *btnCueSpecialSlidesPrev = nullptr;
	QPushButton *btnCueSpecialBlack = nullptr;
	QPushButton *btnCueSpecialRestore = nullptr;
	QPushButton *btnCueSpecialDelay = nullptr;
	QPushButton *btnCueSpecialScope = nullptr;
	QPushButton *btnCueExport = nullptr;
	QPushButton *btnCueImport = nullptr;
	QTimer *cueTimer = nullptr;
	HFRSwitcherPanel *switcher = nullptr; /* 切换台页签（作用域=当前操作画布） */
	QVector<HfrCue> cues;
	int cueIndex = -1;          /* 当前执行到的节目条（-1 = 未开始） */
	bool cueAutoPlaying = false;

	QPushButton *btnSceneNew = nullptr;
	QPushButton *btnSceneUse = nullptr;
	QPushButton *btnSceneDel = nullptr;
	QPushButton *btnSceneColor = nullptr;
	QPushButton *btnSafeArea = nullptr;
	QPushButton *btnStopOne = nullptr;
	QPushButton *btnStopAll = nullptr;
	QLabel *status = nullptr;

	QVector<obs_canvas_t *> canvases;                 /* 本坞创建/持有的用户画布（强引用） */
	QVector<HFRProjection> projections; /* 受管投影记录 */
	QHash<obs_canvas_t *, bool> testSceneAdded;       /* 是否已加测试色场景 */
	QHash<obs_canvas_t *, bool> safeAreaPref;         /* 每画布安全区开关记忆 */
	bool loadingState = false;                        /* 载入中：不触发保存 */
	static HFRConsoleDock *s_instance;
};