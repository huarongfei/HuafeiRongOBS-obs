#pragma once

#include "OBSQTDisplay.hpp"

#include <obs.hpp>

#include <QHash>
#include <QDockWidget>
#include <QPointer>
#include <QVector>

class QComboBox;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QLabel;
class QScreen;

/* ============================================================
 * HuafeiRongOBS 嵌入式多画布 UI（S2）
 *  - 画布行 = 实时缩略预览 + 名称/分辨率（点选行 = 选中该画布为操作对象）
 *  - 显示器自动编号；"投影到选中屏"全屏受管
 *  - "填充测试色块"就地给画布一个可见内容（新画布默认为空→黑屏的验证手段）
 * ============================================================ */

/* 画布行控件：缩略预览 + 文字 */
class HFRCanvasRow : public QWidget {
public:
	HFRCanvasRow(obs_canvas_t *cv, bool recording, bool streaming, QWidget *parent = nullptr);
	~HFRCanvasRow() override;

private:
	static void DrawThumb(void *data, uint32_t cx, uint32_t cy);
	obs_canvas_t *canvas = nullptr;
	OBSQTDisplay *thumb = nullptr;
	bool ready = false;
};

/* 画布投影窗：画布 → 指定编号显示器（全屏、受管） */
class HFRCanvasProjector : public OBSQTDisplay {
public:
	HFRCanvasProjector(obs_canvas_t *canvas, QScreen *screen, QWidget *parent = nullptr);
	~HFRCanvasProjector() override;

	void SetSafeArea(bool on) { safeArea = on; }
	bool HasSafeArea() const { return safeArea; }

private:
	static void Draw(void *data, uint32_t cx, uint32_t cy);
	obs_canvas_t *canvas = nullptr;
	bool ready = false;
	bool safeArea = false;
};

/* 投影记录：画布 → 屏编号（矩阵内受管，可单独停止） */
struct HFRProjection {
	QPointer<HFRCanvasProjector> window;
	obs_canvas_t *canvas = nullptr;
	int screenIndex = -1;
};

/* 输出矩阵 / 导播坞 */
class HFRConsoleDock : public QDockWidget {
public:
	explicit HFRConsoleDock(QWidget *parent = nullptr);
	~HFRConsoleDock() override;

	static HFRConsoleDock *Get() { return s_instance; }

	void RefreshMonitors();
	void RefreshCanvasList();
	void CreateCanvas();
	void RemoveSelectedCanvas();
	void RenameSelectedCanvas();
	void ProjectCanvasToMonitor(obs_canvas_t *cv, int mRow);
	void ProjectSelected();
	void StopProjectionOfSelected();
	void CloseAllProjectors();
	void CleanupForShutdown(); /* 退出前：关投影并释放全部用户画布（obs 仍存活时） */
	void StartStreamingSelected();     /* 把选中画布推流（编码器绑画布 video + 当前推流服务设置） */
	void StopStreamingSelected();      /* 停止选中画布推流 */
	void StopAllStreams();             /* 停止全部画布推流 */
	void StartRecordingSelected();     /* 把选中画布录制到独立文件（编码器绑画布 video） */
	void StopRecordingSelected();      /* 停止选中画布的录制 */
	void StopAllRecordings();          /* 停止全部画布录制（退出/清理时用） */
	void PptOpenTest();               /* PPT：选择 pptx，用 LOK 渲染验证（BMP + 统计） */
	void PptNext();                    /* PPT：下一页 */
	void PptPrev();                    /* PPT：上一页 */
	void ToggleSafeAreaOnSelected(); /* 选中画布的投影：安全区网格开关 */
	void UpdateSceneListForSelection(); /* 场景作用域：列出当前画布的场景 */
	void CreateSceneForSelected();      /* 为当前画布新建场景 */
	void SwitchToSelectedScene();       /* 把当前画布切到选中场景 */
	void DeleteSelectedScene();         /* 删除当前画布的选中场景 */
	void FillColorIntoSelectedScene();  /* 给选中场景加一块铺满的色块（演示内容） */
	QString StateFilePath() const;   /* profile 目录下的状态文件路径 */
	void SaveState();                /* 持久化画布/投影布局到 profile 目录 JSON */
	void LoadState();                /* 启动时恢复 */
	void SetTestColorOnSelected(); /* 给选中画布就地填一块测试色 */
	void SetCanvasTestColor(obs_canvas_t *cv);

private:
	obs_canvas_t *CanvasFromRow(int row) const;
	void AddCanvasRowItem(obs_canvas_t *cv, const QString &label);

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
		obs_encoder_t *aenc = nullptr;
		QString path;
	};
	QHash<obs_canvas_t *, HfrRecording> recordings; /* 每画布一路录制 */

	struct HfrStream {
		obs_service_t *svc = nullptr;
		obs_output_t *out = nullptr;
		obs_encoder_t *venc = nullptr;
		obs_encoder_t *aenc = nullptr;
	};
	QHash<obs_canvas_t *, HfrStream> streams; /* 每画布一路推流 */

	QPushButton *btnPptOpen = nullptr;
	QPushButton *btnPptPrev = nullptr;
	QPushButton *btnPptNext = nullptr;
	QPushButton *btnPresenter = nullptr;
	QComboBox *encCombo = nullptr;
	QPushButton *btnStreamStart = nullptr;
	QPushButton *btnStreamStop = nullptr;
	QPushButton *btnStreamStopAll = nullptr;
	QPushButton *btnRecStart = nullptr;
	QPushButton *btnRecStop = nullptr;
	QPushButton *btnRecStopAll = nullptr;
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