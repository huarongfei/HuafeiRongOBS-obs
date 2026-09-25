#include "HFRConsole.hpp"

#include <QAction>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScreen>
#include <QThread>
#include <QVBoxLayout>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QVariant>

#include <obs-frontend-api.h>

#include "HFRPresenter.hpp"
#include "HFRPpt.hpp"

#include <vector>

#include <obs-module.h>

HFRConsoleDock *HFRConsoleDock::s_instance = nullptr;

/* PPT 自检辅助（定义在文件下部） */
static HFRPptDocument &PptSelfTestDoc();
static void WriteBmp32File(const QString &path, uint32_t w, uint32_t h, const std::vector<uint8_t> &bgra);
static void PptRenderAndReport(QLabel *status);

/* PPT 相关入口声明见 HFRPpt.hpp（HfrPptTestOutputDir / HfrRegisterPptSource） */

static void HfrLog(const char *msg)
{
	blog(LOG_INFO, "[HFR] %s", msg);
}

/* ---------------------------- HFRCanvasRow ---------------------------- */

HFRCanvasRow::HFRCanvasRow(obs_canvas_t *cv, bool recording, bool streaming, QWidget *parent)
	: QWidget(parent), canvas(cv)
{
	QHBoxLayout *layout = new QHBoxLayout(this);
	layout->setContentsMargins(2, 2, 2, 2);
	layout->setSpacing(6);

	thumb = new OBSQTDisplay(this);
	thumb->setFixedSize(160, 90);
	thumb->setMinimumSize(160, 90);
	layout->addWidget(thumb);

	QVBoxLayout *txt = new QVBoxLayout();
	QLabel *name = new QLabel(this);
	QString title = QString::fromUtf8(obs_canvas_get_name(cv));
	QString prefix;
	if (streaming) {
		prefix += QStringLiteral("● LIVE  ");
	}
	if (recording) {
		prefix += QStringLiteral("● REC  ");
	}
	title = prefix + title;
	if (!prefix.isEmpty()) {
		name->setStyleSheet(QStringLiteral("font-weight:bold; color:#e04040;"));
	} else {
		name->setStyleSheet(QStringLiteral("font-weight:bold;"));
	}
	name->setText(title);
	txt->addWidget(name);

	struct obs_video_info ovi;
	QString sizeText = QStringLiteral("?");
	if (obs_canvas_get_video_info(cv, &ovi)) {
		sizeText = QStringLiteral("%1x%2").arg(ovi.base_width).arg(ovi.base_height);
	}
	QLabel *size = new QLabel(sizeText, this);
	size->setStyleSheet(QStringLiteral("color:#888;"));
	txt->addWidget(size);
	layout->addLayout(txt, 1);

	auto addDraw = [this]() {
		obs_display_add_draw_callback(thumb->GetDisplay(), &HFRCanvasRow::DrawThumb, this);
	};
	connect(thumb, &OBSQTDisplay::DisplayCreated, this, addDraw);
	ready = true;
}

HFRCanvasRow::~HFRCanvasRow()
{
	if (thumb && thumb->GetDisplay()) {
		obs_display_remove_draw_callback(thumb->GetDisplay(), &HFRCanvasRow::DrawThumb, this);
	}
}

void HFRCanvasRow::DrawThumb(void *data, uint32_t, uint32_t)
{
	HFRCanvasRow *row = static_cast<HFRCanvasRow *>(data);
	if (!row || !row->ready || !row->canvas || !row->thumb) {
		return;
	}
	obs_render_canvas_texture(row->canvas);
}

/* ------------------------- HFRCanvasProjector ------------------------- */

HFRCanvasProjector::HFRCanvasProjector(obs_canvas_t *canvas_, QScreen *screen, QWidget *parent)
	: OBSQTDisplay(parent), canvas(canvas_)
{
	int idx = QGuiApplication::screens().indexOf(screen);
	QString title;
	if (canvas && screen) {
		title = QStringLiteral("%1 → 屏%2 (%3)")
				.arg(QString::fromUtf8(obs_canvas_get_name(canvas)))
				.arg(idx)
				.arg(screen->name());
	} else {
		title = QStringLiteral("画布投影");
	}
	setWindowTitle(title);
	setAttribute(Qt::WA_DeleteOnClose, true);
	setAttribute(Qt::WA_QuitOnClose, false);

	QAction *escapeAction = new QAction(this);
	escapeAction->setShortcut(Qt::Key_Escape);
	addAction(escapeAction);
	connect(escapeAction, &QAction::triggered, this, &QWidget::close);

	auto addDraw = [this]() {
		obs_display_add_draw_callback(GetDisplay(), &HFRCanvasProjector::Draw, this);
		obs_display_set_background_color(GetDisplay(), 0x000000);
	};
	connect(this, &OBSQTDisplay::DisplayCreated, this, addDraw);

	ready = true;
	if (screen) {
		setGeometry(screen->geometry());
		showFullScreen();
	} else {
		resize(640, 360);
		show();
	}
	raise();
	activateWindow();
}

HFRCanvasProjector::~HFRCanvasProjector()
{
	if (GetDisplay()) {
		obs_display_remove_draw_callback(GetDisplay(), &HFRCanvasProjector::Draw, this);
	}
}

/* 安全区矩形叠加（ratio=内缩比例：0.05→90% 标题区，0.10→80% 动作区） */
static void DrawSafeAreaRect(uint32_t cx, uint32_t cy, float ratio, uint32_t color)
{
	if (cx == 0 || cy == 0) {
		return;
	}
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *colorParam = gs_effect_get_param_by_name(solid, "color");
	gs_effect_set_color(colorParam, color);

	const float m = ratio;
	const float x0 = m * (float)cx;
	const float y0 = m * (float)cy;
	const float w = (1.0f - 2.0f * m) * (float)cx;
	const float h = (1.0f - 2.0f * m) * (float)cy;
	const float t = 2.0f;
	if (w <= 0.0f || h <= 0.0f) {
		return;
	}

	auto bar = [&](float bx, float by, float bw, float bh) {
		gs_matrix_push();
		gs_matrix_translate3f(bx, by, 0.0f);
		while (gs_effect_loop(solid, "Solid")) {
			gs_draw_sprite(nullptr, 0, (uint32_t)bw, (uint32_t)bh);
		}
		gs_matrix_pop();
	};

	bar(x0, y0, w, t);
	bar(x0, y0 + h - t, w, t);
	bar(x0, y0, t, h);
	bar(x0 + w - t, y0, t, h);
}

void HFRCanvasProjector::Draw(void *data, uint32_t cx, uint32_t cy)
{
	HFRCanvasProjector *window = static_cast<HFRCanvasProjector *>(data);
	if (!window || !window->ready || !window->canvas) {
		return;
	}
	obs_render_canvas_texture(window->canvas);

	if (window->HasSafeArea()) {
		gs_viewport_push();
		gs_projection_push();
		gs_set_viewport(0, 0, (int)cx, (int)cy);
		gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
		DrawSafeAreaRect(cx, cy, 0.05f, 0x66FFFFFF); /* 90% */
		DrawSafeAreaRect(cx, cy, 0.10f, 0x66FF4040); /* 80% */
		gs_projection_pop();
		gs_viewport_pop();
	}
}

/* --------------------------- HFRConsoleDock --------------------------- */

HFRConsoleDock::HFRConsoleDock(QWidget *parent) : QDockWidget(parent)
{
	s_instance = this;
	setWindowTitle(QStringLiteral("输出矩阵（画布）"));
	setObjectName(QStringLiteral("hfrOutputMatrixDock"));

	QWidget *panel = new QWidget(this);
	QVBoxLayout *layout = new QVBoxLayout(panel);
	layout->setContentsMargins(4, 4, 4, 4);

	layout->addWidget(new QLabel(QStringLiteral("显示器（编号）"), panel));
	monitorList = new QListWidget(panel);
	monitorList->setMaximumHeight(110);
	layout->addWidget(monitorList);

	layout->addWidget(new QLabel(QStringLiteral("画布（点选行 = 当前操作画布）"), panel));
	canvasList = new QListWidget(panel);
	canvasList->setUniformItemSizes(false);
	layout->addWidget(canvasList);

	layout->addWidget(new QLabel(QStringLiteral("场景（当前画布）"), panel));
	sceneList = new QListWidget(panel);
	sceneList->setMaximumHeight(120);
	layout->addWidget(sceneList);

	QHBoxLayout *scRow = new QHBoxLayout();
	btnSceneNew = new QPushButton(QStringLiteral("+场景"), panel);
	btnSceneUse = new QPushButton(QStringLiteral("切到此场景"), panel);
	btnSceneDel = new QPushButton(QStringLiteral("删场景"), panel);
	btnSceneColor = new QPushButton(QStringLiteral("给场景加色块"), panel);
	scRow->addWidget(btnSceneNew);
	scRow->addWidget(btnSceneUse);
	scRow->addWidget(btnSceneDel);
	scRow->addWidget(btnSceneColor);
	layout->addLayout(scRow);

	QHBoxLayout *btnRow = new QHBoxLayout();
	btnRefresh = new QPushButton(QStringLiteral("刷新"), panel);
	btnNew = new QPushButton(QStringLiteral("+ 新建画布"), panel);
	btnRemove = new QPushButton(QStringLiteral("删除"), panel);
	btnRename = new QPushButton(QStringLiteral("重命名"), panel);
	btnRow->addWidget(btnRefresh);
	btnRow->addWidget(btnNew);
	btnRow->addWidget(btnRename);
	btnRow->addWidget(btnRemove);
	layout->addLayout(btnRow);

	QHBoxLayout *actRow = new QHBoxLayout();
	btnTest = new QPushButton(QStringLiteral("填充测试色"), panel);
	btnSafeArea = new QPushButton(QStringLiteral("安全区"), panel);
	btnProject = new QPushButton(QStringLiteral("▶投影选中屏"), panel);
	btnStopOne = new QPushButton(QStringLiteral("■停本画布"), panel);
	btnStopAll = new QPushButton(QStringLiteral("■全部关闭"), panel);
	actRow->addWidget(btnTest);
	actRow->addWidget(btnSafeArea);
	actRow->addWidget(btnProject);
	actRow->addWidget(btnStopOne);
	actRow->addWidget(btnStopAll);
	layout->addLayout(actRow);

	QHBoxLayout *pptRow = new QHBoxLayout();
	btnPptOpen = new QPushButton(QStringLiteral("打开PPT"), panel);
	btnPptPrev = new QPushButton(QStringLiteral("PPT上一页"), panel);
	btnPptNext = new QPushButton(QStringLiteral("PPT下一页"), panel);
	btnPresenter = new QPushButton(QStringLiteral("讲者视图"), panel);
	pptRow->addWidget(btnPptOpen);
	pptRow->addWidget(btnPptPrev);
	pptRow->addWidget(btnPptNext);
	pptRow->addWidget(btnPresenter);
	layout->addLayout(pptRow);

	QHBoxLayout *liveRow = new QHBoxLayout();
	btnStreamStart = new QPushButton(QStringLiteral("●推流该画布"), panel);
	btnStreamStop = new QPushButton(QStringLiteral("■停推流"), panel);
	btnStreamStopAll = new QPushButton(QStringLiteral("■全部停推流"), panel);
	liveRow->addWidget(btnStreamStart);
	liveRow->addWidget(btnStreamStop);
	liveRow->addWidget(btnStreamStopAll);
	layout->addLayout(liveRow);

	QHBoxLayout *outRow = new QHBoxLayout();
	encCombo = new QComboBox(panel);
	{
		const char *id = nullptr;
		for (size_t i = 0; obs_enum_encoder_types(i, &id); i++) {
			if (!id) {
				continue;
			}
			const QString sid = QString::fromUtf8(id);
			if (sid.contains(QStringLiteral("264")) || sid == QStringLiteral("jim_nvenc")) {
				encCombo->addItem(sid, sid);
			}
		}
		const int def = encCombo->findData(QStringLiteral("obs_x264"));
		if (def >= 0) {
			encCombo->setCurrentIndex(def);
		}
	}
	btnRecStart = new QPushButton(QStringLiteral("●录制该画布"), panel);
	btnRecStop = new QPushButton(QStringLiteral("■停录制"), panel);
	btnRecStopAll = new QPushButton(QStringLiteral("■全部停录"), panel);
	outRow->addWidget(encCombo);
	outRow->addWidget(btnRecStart);
	outRow->addWidget(btnRecStop);
	outRow->addWidget(btnRecStopAll);
	layout->addLayout(outRow);

	status = new QLabel(QStringLiteral("就绪"), panel);
	status->setWordWrap(true);
	status->setMaximumHeight(60);
	layout->addWidget(status);

	setWidget(panel);

	connect(btnRefresh, &QPushButton::clicked, this, [this]() {
		RefreshMonitors();
		RefreshCanvasList();
	});
	connect(btnNew, &QPushButton::clicked, this, [this]() { CreateCanvas(); });
	connect(btnRemove, &QPushButton::clicked, this, [this]() { RemoveSelectedCanvas(); });
	connect(btnRename, &QPushButton::clicked, this, [this]() { RenameSelectedCanvas(); });
	connect(btnTest, &QPushButton::clicked, this, [this]() { SetTestColorOnSelected(); });
	connect(btnSafeArea, &QPushButton::clicked, this, [this]() { ToggleSafeAreaOnSelected(); });
	connect(btnSceneNew, &QPushButton::clicked, this, [this]() { CreateSceneForSelected(); });
	connect(btnSceneUse, &QPushButton::clicked, this, [this]() { SwitchToSelectedScene(); });
	connect(btnSceneDel, &QPushButton::clicked, this, [this]() { DeleteSelectedScene(); });
	connect(btnSceneColor, &QPushButton::clicked, this, [this]() { FillColorIntoSelectedScene(); });
	connect(canvasList, &QListWidget::currentRowChanged, this, [this](int) { UpdateSceneListForSelection(); });
	connect(btnProject, &QPushButton::clicked, this, [this]() { ProjectSelected(); });
	connect(btnStopOne, &QPushButton::clicked, this, [this]() { StopProjectionOfSelected(); });
	connect(btnRecStart, &QPushButton::clicked, this, [this]() { StartRecordingSelected(); });
	connect(btnRecStop, &QPushButton::clicked, this, [this]() { StopRecordingSelected(); });
	connect(btnPptOpen, &QPushButton::clicked, this, [this]() { PptOpenTest(); });
	connect(btnPptPrev, &QPushButton::clicked, this, [this]() { PptPrev(); });
	connect(btnPptNext, &QPushButton::clicked, this, [this]() { PptNext(); });
	connect(btnPresenter, &QPushButton::clicked, this, []() { HfrOpenPresenterWindow(); });
	connect(btnStreamStart, &QPushButton::clicked, this, [this]() { StartStreamingSelected(); });
	connect(btnStreamStop, &QPushButton::clicked, this, [this]() { StopStreamingSelected(); });
	connect(btnStreamStopAll, &QPushButton::clicked, this, [this]() {
		StopAllStreams();
		RefreshCanvasList();
		status->setText(QStringLiteral("已停止全部画布推流"));
	});
	connect(btnRecStopAll, &QPushButton::clicked, this, [this]() {
		StopAllRecordings();
		RefreshCanvasList();
		status->setText(QStringLiteral("已停止全部画布录制"));
	});
	connect(btnStopAll, &QPushButton::clicked, this, [this]() { CloseAllProjectors(); });
	connect(canvasList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
		int row = canvasList->row(item);
		obs_canvas_t *cv = CanvasFromRow(row);
		if (!cv) {
			return;
		}
		const auto screens = QGuiApplication::screens();
		int primary = 0;
		for (int i = 0; i < screens.size(); i++) {
			if (screens.at(i) == QGuiApplication::primaryScreen()) {
				primary = i;
				break;
			}
		}
		ProjectCanvasToMonitor(cv, primary);
	});

	RefreshMonitors();
	RefreshCanvasList();
	UpdateSceneListForSelection();

	LoadState();
	UpdateSceneListForSelection();

	/* 无人值守自检：设置 HFR_PPT_AUTOTEST=<pptx 路径> 后启动即渲染一次并写日志 */
	if (qEnvironmentVariableIsSet("HFR_PPT_AUTOTEST")) {
		const QString pptPath = qEnvironmentVariable("HFR_PPT_AUTOTEST");
		QTimer::singleShot(5000, [pptPath]() {
			QString err;
			HFRPptDocument &doc = PptSelfTestDoc();
			blog(LOG_INFO, "[HFR-PPT] autotest: open %s", pptPath.toUtf8().constData());
			if (!doc.Open(pptPath, &err)) {
				blog(LOG_WARNING, "[HFR-PPT] autotest open failed: %s", err.toUtf8().constData());
				return;
			}
			std::vector<uint8_t> buf;
			if (doc.RenderCurrentPart(1280, 720, buf, &err)) {
				const QString out = HfrPptTestOutputDir() + QStringLiteral("/hfr_ppt_autotest.bmp");
				WriteBmp32File(out, 1280, 720, buf);
				blog(LOG_INFO, "[HFR-PPT] autotest OK: 第 %d/%d 页 -> %s", doc.CurrentPart() + 1,
				     doc.PartCount(), out.toUtf8().constData());
			} else {
				blog(LOG_WARNING, "[HFR-PPT] autotest render failed: %s", err.toUtf8().constData());
			}
		});
	}
}

HFRConsoleDock::~HFRConsoleDock()
{
	CleanupForShutdown();
	s_instance = nullptr;
}

/* 往指定场景加入一块按画布分辨率铺满的色块 */
static void AddColorIntoScene(obs_canvas_t *cv, obs_scene_t *sc, const QString &srcName)
{
	if (!cv || !sc) {
		return;
	}
	struct obs_video_info ovi = {};
	uint32_t w = 1280, h = 720;
	if (obs_canvas_get_video_info(cv, &ovi) && ovi.base_width && ovi.base_height) {
		w = ovi.base_width;
		h = ovi.base_height;
	}
	obs_data_t *s = obs_data_create();
	obs_data_set_int(s, "color", 0xFF00C8FF);
	obs_data_set_int(s, "width", (long long)w);
	obs_data_set_int(s, "height", (long long)h);
	obs_source_t *cs = obs_source_create("color_source_v3", srcName.toUtf8().constData(), s, nullptr);
	obs_data_release(s);
	if (!cs) {
		return;
	}
	obs_sceneitem_t *it = obs_scene_add(sc, cs);
	if (it) {
		obs_sceneitem_set_bounds_type(it, OBS_BOUNDS_STRETCH);
		struct vec2 sz = {(float)w, (float)h};
		obs_sceneitem_set_bounds(it, &sz);
		struct vec2 pos = {0.0f, 0.0f};
		obs_sceneitem_set_pos(it, &pos);
	}
	obs_source_release(cs);
}

/* 枚举回调：收集画布内场景（先收集后移除，避免遍历中修改） */
static bool CollectSceneCallback(void *param, obs_source_t *sceneSource)
{
	auto *scenes = static_cast<std::vector<obs_scene_t *> *>(param);
	if (obs_scene_t *sc = obs_scene_from_source(sceneSource)) {
		scenes->push_back(sc);
	}
	return true;
}

/* 运行时删除单个画布：清通道 → 移除其场景 → 释放画布 */
static void TeardownUserCanvas(obs_canvas_t *cv)
{
	if (!cv || cv == obs_get_main_canvas()) {
		return;
	}
	obs_canvas_set_channel(cv, 0, nullptr);
	std::vector<obs_scene_t *> scenes;
	obs_canvas_enum_scenes(cv, CollectSceneCallback, &scenes);
	for (obs_scene_t *sc : scenes) {
		obs_canvas_scene_remove(sc);
	}
	obs_canvas_remove(cv);
	obs_canvas_release(cv);
}

void HFRConsoleDock::CleanupForShutdown()
{
	StopAllStreams();
	StopAllRecordings();
	CloseAllProjectors();
	for (obs_canvas_t *cv : canvases) {
		TeardownUserCanvas(cv);
	}
	canvases.clear();
	testSceneAdded.clear();
	safeAreaPref.clear();

	/* 关键：在 obs_shutdown 之前把延迟销毁队列排空（官方 API），
	 * 否则画布场景/源的销毁会与 obs_free_data 竞争（曾致 c0000005） */
	obs_wait_for_destroy_queue();
	obs_wait_for_destroy_queue();
	HfrLog("cleanup: scenes removed, canvases released, destroy queue drained");
}

void HFRConsoleDock::RefreshMonitors()
{
	monitorList->clear();
	const auto screens = QGuiApplication::screens();
	for (int i = 0; i < screens.size(); i++) {
		QScreen *s = screens.at(i);
		QString label = QStringLiteral("#%1  %2  %3x%4%5")
					.arg(i)
					.arg(s->name())
					.arg(s->geometry().width())
					.arg(s->geometry().height())
					.arg(s == QGuiApplication::primaryScreen() ? QStringLiteral("  [主]") : QString());
		monitorList->addItem(label);
	}
	status->setText(QStringLiteral("显示器 %1 块；接好外接屏后点“刷新”重新编号").arg(screens.size()));
}

void HFRConsoleDock::AddCanvasRowItem(obs_canvas_t *cv, const QString &label)
{
	Q_UNUSED(label);
	QListWidgetItem *item = new QListWidgetItem(canvasList);
	item->setData(Qt::UserRole, QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(cv)));
	HFRCanvasRow *row = new HFRCanvasRow(cv, recordings.contains(cv), streams.contains(cv), canvasList);
	item->setSizeHint(row->sizeHint());
	canvasList->setItemWidget(item, row);
}

void HFRConsoleDock::RefreshCanvasList()
{
	canvasList->clear();
	obs_canvas_t *mainCanvas = obs_get_main_canvas();
	if (mainCanvas) {
		AddCanvasRowItem(mainCanvas, QString());
		obs_canvas_release(mainCanvas);
	}
	for (obs_canvas_t *cv : canvases) {
		if (cv) {
			AddCanvasRowItem(cv, QString());
		}
	}
	if (canvasList->count() > 0) {
		canvasList->setCurrentRow(0);
	}
}

obs_canvas_t *HFRConsoleDock::CanvasFromRow(int row) const
{
	QListWidgetItem *item = canvasList->item(row);
	if (!item) {
		return nullptr;
	}
	return reinterpret_cast<obs_canvas_t *>(
		static_cast<quintptr>(item->data(Qt::UserRole).toULongLong()));
}

void HFRConsoleDock::CreateCanvas()
{
	bool okName = false;
	QString name = QInputDialog::getText(this, QStringLiteral("新建画布"),
					     QStringLiteral("画布名称"), QLineEdit::Normal,
					     QStringLiteral("画布"), &okName);
	if (!okName || name.trimmed().isEmpty()) {
		return;
	}

	const QStringList resList = {QStringLiteral("1280x720"), QStringLiteral("1920x1080"),
				     QStringLiteral("960x540"), QStringLiteral("2560x1440")};
	bool okRes = false;
	QString res = QInputDialog::getItem(this, QStringLiteral("新建画布"),
					    QStringLiteral("基础分辨率（帧率随全局）"),
					    resList, 0, false, &okRes);
	if (!okRes || res.isEmpty()) {
		return;
	}
	QStringList wh = res.split('x');
	if (wh.size() != 2) {
		return;
	}

	struct obs_video_info ovi = {};
	obs_get_video_info(&ovi);
	ovi.base_width = wh.at(0).toUInt();
	ovi.base_height = wh.at(1).toUInt();

	obs_canvas_t *cv = obs_canvas_create(name.trimmed().toUtf8().constData(), &ovi, ACTIVATE | SCENE_REF);
	if (!cv) {
		status->setText(QStringLiteral("新建画布失败"));
		return;
	}
	canvases.append(cv);
	RefreshCanvasList();
	canvasList->setCurrentRow(canvasList->count() - 1);
	SaveState();
	HfrLog("created canvas with chosen name/resolution");
	status->setText(QStringLiteral("已新建画布：%1（点“填充测试色”即可见内容）")
				.arg(QString::fromUtf8(obs_canvas_get_name(cv))));
}

void HFRConsoleDock::RenameSelectedCanvas()
{
	int row = canvasList->currentRow();
	obs_canvas_t *cv = CanvasFromRow(row);
	if (!cv) {
		status->setText(QStringLiteral("请先点选画布"));
		return;
	}
	if (cv == obs_get_main_canvas()) {
		status->setText(QStringLiteral("主画布不可重命名"));
		return;
	}
	bool ok = false;
	QString name = QInputDialog::getText(this, QStringLiteral("重命名画布"),
					      QStringLiteral("画布名称"),
					      QLineEdit::Normal,
					      QString::fromUtf8(obs_canvas_get_name(cv)), &ok);
	if (!ok || name.trimmed().isEmpty()) {
		return;
	}
	obs_canvas_set_name(cv, name.trimmed().toUtf8().constData());
	RefreshCanvasList();
	SaveState();
	HfrLog("renamed canvas");
}

/* ---------------- 场景作用域（当前画布） ---------------- */

static bool CollectSceneSourcesCallback(void *param, obs_source_t *sceneSource)
{
	auto *vec = static_cast<std::vector<obs_source_t *> *>(param);
	vec->push_back(sceneSource);
	return true;
}

void HFRConsoleDock::UpdateSceneListForSelection()
{
	if (!sceneList) {
		return;
	}
	sceneList->clear();
	obs_canvas_t *cv = CanvasFromRow(canvasList->currentRow());
	if (!cv) {
		return;
	}
	std::vector<obs_source_t *> scenes;
	obs_canvas_enum_scenes(cv, CollectSceneSourcesCallback, &scenes);

	obs_source_t *cur = obs_canvas_get_channel(cv, 0);
	QString curName = cur ? QString::fromUtf8(obs_source_get_name(cur)) : QString();

	for (obs_source_t *s : scenes) {
		const QString name = QString::fromUtf8(obs_source_get_name(s));
		QListWidgetItem *item = new QListWidgetItem(
			(name == curName ? QStringLiteral("▶ ") : QStringLiteral("   ")) + name, sceneList);
		item->setData(Qt::UserRole, QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(s)));
		if (name == curName) {
			sceneList->setCurrentItem(item);
		}
	}
}

void HFRConsoleDock::CreateSceneForSelected()
{
	obs_canvas_t *cv = CanvasFromRow(canvasList->currentRow());
	if (!cv) {
		status->setText(QStringLiteral("请先点选画布"));
		return;
	}
	bool ok = false;
	QString name = QInputDialog::getText(this, QStringLiteral("新建场景"), QStringLiteral("场景名称"),
					     QLineEdit::Normal,
					     QString::fromUtf8(obs_canvas_get_name(cv)) + QStringLiteral(" · 场景"), &ok);
	if (!ok || name.trimmed().isEmpty()) {
		return;
	}
	obs_scene_t *sc = obs_canvas_scene_create(cv, name.trimmed().toUtf8().constData());
	if (!sc) {
		status->setText(QStringLiteral("创建场景失败（名称冲突？）"));
		return;
	}
	UpdateSceneListForSelection();
	SaveState();
	status->setText(QStringLiteral("已新建场景：%1").arg(name.trimmed()));
}

void HFRConsoleDock::SwitchToSelectedScene()
{
	obs_canvas_t *cv = CanvasFromRow(canvasList->currentRow());
	if (!cv) {
		status->setText(QStringLiteral("请先点选画布"));
		return;
	}
	QListWidgetItem *item = sceneList->currentItem();
	if (!item) {
		status->setText(QStringLiteral("请先选中场景"));
		return;
	}
	obs_source_t *scSrc = reinterpret_cast<obs_source_t *>(
		static_cast<quintptr>(item->data(Qt::UserRole).toULongLong()));
	if (!scSrc) {
		return;
	}
	obs_canvas_set_channel(cv, 0, scSrc);
	UpdateSceneListForSelection();
	SaveState();
	status->setText(QStringLiteral("已切换场景：%1").arg(QString::fromUtf8(obs_source_get_name(scSrc))));
	HfrLog("scene switched on canvas");
}

void HFRConsoleDock::DeleteSelectedScene()
{
	obs_canvas_t *cv = CanvasFromRow(canvasList->currentRow());
	QListWidgetItem *item = sceneList->currentItem();
	if (!cv || !item) {
		status->setText(QStringLiteral("请先选中画布与场景"));
		return;
	}
	obs_source_t *scSrc = reinterpret_cast<obs_source_t *>(
		static_cast<quintptr>(item->data(Qt::UserRole).toULongLong()));
	obs_scene_t *sc = scSrc ? obs_scene_from_source(scSrc) : nullptr;
	if (!sc) {
		return;
	}
	/* 若删除的是当前场景，先断开通道，避免引用悬空 */
	if (obs_canvas_get_channel(cv, 0) == scSrc) {
		obs_canvas_set_channel(cv, 0, nullptr);
	}
	obs_canvas_scene_remove(sc);
	UpdateSceneListForSelection();
	SaveState();
	status->setText(QStringLiteral("已删除场景"));
}

void HFRConsoleDock::FillColorIntoSelectedScene()
{
	obs_canvas_t *cv = CanvasFromRow(canvasList->currentRow());
	QListWidgetItem *item = sceneList->currentItem();
	if (!cv || !item) {
		status->setText(QStringLiteral("请先选中画布与场景"));
		return;
	}
	obs_source_t *scSrc = reinterpret_cast<obs_source_t *>(
		static_cast<quintptr>(item->data(Qt::UserRole).toULongLong()));
	obs_scene_t *sc = scSrc ? obs_scene_from_source(scSrc) : nullptr;
	if (!sc) {
		return;
	}

	AddColorIntoScene(cv, sc, QString::fromUtf8(obs_source_get_name(scSrc)) + QStringLiteral(" · 色块"));
	status->setText(QStringLiteral("已给场景加色块"));
	HfrLog("color added into canvas scene");
}

/* ---------------- 编码器公共创建（绑画布 video / 全局音频） ---------------- */

static obs_encoder_t *CreateCanvasVideoEncoder(obs_canvas_t *cv, const QString &encId, QString *usedId)
{
	struct obs_video_info ovi = {};
	uint32_t w = 1280, h = 720;
	if (obs_canvas_get_video_info(cv, &ovi) && ovi.base_width && ovi.base_height) {
		w = ovi.base_width;
		h = ovi.base_height;
	}
	UNUSED_PARAMETER(w);
	UNUSED_PARAMETER(h);

	const QString canvasName = QString::fromUtf8(obs_canvas_get_name(cv));
	const QString nm = canvasName + QStringLiteral(" · 视频");

	auto make = [&](const QString &id) -> obs_encoder_t * {
		obs_data_t *vs = obs_data_create();
		obs_data_set_string(vs, "rate_control", "CBR");
		obs_data_set_int(vs, "bitrate", 6000);
		obs_data_set_int(vs, "keyint_sec", 2);
		if (id == QStringLiteral("obs_x264")) {
			obs_data_set_string(vs, "preset", "veryfast");
			obs_data_set_string(vs, "profile", "high");
		}
		obs_encoder_t *e = obs_video_encoder_create(id.toUtf8().constData(), nm.toUtf8().constData(), vs, nullptr);
		obs_data_release(vs);
		return e;
	};

	QString id = encId.isEmpty() ? QStringLiteral("obs_x264") : encId;
	obs_encoder_t *e = make(id);
	if (!e && id != QStringLiteral("obs_x264")) {
		id = QStringLiteral("obs_x264");
		e = make(id);
	}
	if (e && usedId) {
		*usedId = id;
	}
	return e;
}

static obs_encoder_t *CreateCanvasAudioEncoder(obs_canvas_t *cv)
{
	const QString canvasName = QString::fromUtf8(obs_canvas_get_name(cv));
	const QString nm = canvasName + QStringLiteral(" · 音频");
	obs_data_t *as = obs_data_create();
	obs_data_set_int(as, "bitrate", 160);
	obs_encoder_t *e = obs_audio_encoder_create("ffmpeg_aac", nm.toUtf8().constData(), as, 0, nullptr);
	obs_data_release(as);
	if (e) {
		obs_encoder_set_audio(e, obs_get_audio());
	}
	return e;
}

/* ---------------- 每画布独立录制（编码器绑定画布 video） ---------------- */

void HFRConsoleDock::StartRecordingSelected()
{
	obs_canvas_t *cv = CanvasFromRow(canvasList->currentRow());
	if (!cv) {
		status->setText(QStringLiteral("请先点选画布"));
		return;
	}
	if (cv == obs_get_main_canvas()) {
		status->setText(QStringLiteral("主画布请用 OBS 自带录制按钮；这里录制用户画布"));
		return;
	}
	if (recordings.contains(cv)) {
		status->setText(QStringLiteral("该画布已在录制"));
		return;
	}

	video_t *video = obs_canvas_get_video(cv);
	if (!video) {
		status->setText(QStringLiteral("该画布没有视频输出"));
		return;
	}

	char *profilePath = obs_frontend_get_current_profile_path();
	QString dir = profilePath ? QString::fromUtf8(profilePath) : QString();
	if (profilePath) {
		bfree(profilePath);
	}
	if (dir.isEmpty()) {
		status->setText(QStringLiteral("取不到 profile 路径"));
		return;
	}
	const QString canvasName = QString::fromUtf8(obs_canvas_get_name(cv));
	const QString ts = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
	const QString file = dir + QStringLiteral("/hfr_") + canvasName + QStringLiteral("_") + ts + QStringLiteral(".mkv");

	QString encId = encCombo ? encCombo->currentData().toString() : QStringLiteral("obs_x264");
	QString usedEnc;
	obs_encoder_t *venc = CreateCanvasVideoEncoder(cv, encId, &usedEnc);
	if (!venc) {
		status->setText(QStringLiteral("创建视频编码器失败（无可用 H.264 编码器）"));
		return;
	}
	encId = usedEnc;
	obs_encoder_set_video(venc, video);

	obs_encoder_t *aenc = CreateCanvasAudioEncoder(cv);

	obs_data_t *os = obs_data_create();
	obs_data_set_string(os, "path", file.toUtf8().constData());
	obs_data_t *fmt = obs_data_create();
	obs_data_set_string(fmt, "extension", "mkv");
	obs_data_set_obj(os, "format", fmt);
	obs_data_release(fmt);

	QString oname = canvasName + QStringLiteral(" · 录制");
	obs_output_t *out = obs_output_create("ffmpeg_muxer", oname.toUtf8().constData(), os, nullptr);
	obs_data_release(os);
	if (!out) {
		obs_encoder_release(venc);
		if (aenc) {
			obs_encoder_release(aenc);
		}
		status->setText(QStringLiteral("创建输出失败（ffmpeg_muxer 不可用？）"));
		return;
	}
	obs_output_set_media(out, video, aenc ? obs_get_audio() : nullptr);
	obs_output_set_video_encoder(out, venc);
	if (aenc) {
		obs_output_set_audio_encoder(out, aenc, 0);
	}

	if (!obs_output_start(out)) {
		const char *err = obs_output_get_last_error(out);
		status->setText(QStringLiteral("开始录制失败：%1").arg(err ? QString::fromUtf8(err) : QStringLiteral("未知")));
		obs_output_release(out);
		obs_encoder_release(venc);
		if (aenc) {
			obs_encoder_release(aenc);
		}
		return;
	}

	HfrRecording rec;
	rec.out = out;
	rec.venc = venc;
	rec.aenc = aenc;
	rec.path = file;
	recordings.insert(cv, rec);
	status->setText(QStringLiteral("录制中：%1 → %2（编码器 %3）").arg(canvasName).arg(QFileInfo(file).fileName()).arg(encId));
	RefreshCanvasList();
	HfrLog("canvas recording started");
}

void HFRConsoleDock::StopRecordingSelected()
{
	obs_canvas_t *cv = CanvasFromRow(canvasList->currentRow());
	if (!cv || !recordings.contains(cv)) {
		status->setText(QStringLiteral("该画布没有在录制"));
		return;
	}
	HfrRecording rec = recordings.take(cv);
	const QString name = QFileInfo(rec.path).fileName();
	if (rec.out) {
		obs_output_stop(rec.out);
		obs_output_release(rec.out);
	}
	if (rec.venc) {
		obs_encoder_release(rec.venc);
	}
	if (rec.aenc) {
		obs_encoder_release(rec.aenc);
	}
	status->setText(QStringLiteral("已停止录制：%1").arg(name));
	RefreshCanvasList();
	HfrLog("canvas recording stopped");
}

void HFRConsoleDock::StopAllRecordings()
{
	const auto keys = recordings.keys();
	for (obs_canvas_t *cv : keys) {
		if (recordings.contains(cv)) {
			HfrRecording rec = recordings.take(cv);
			if (rec.out) {
				obs_output_stop(rec.out);
				obs_output_release(rec.out);
			}
			if (rec.venc) {
				obs_encoder_release(rec.venc);
			}
			if (rec.aenc) {
				obs_encoder_release(rec.aenc);
			}
		}
	}
}

/* ---------------- 每画布推流（编码器绑画布 video + 当前推流服务） ---------------- */

void HFRConsoleDock::StartStreamingSelected()
{
	obs_canvas_t *cv = CanvasFromRow(canvasList->currentRow());
	if (!cv) {
		status->setText(QStringLiteral("请先点选画布"));
		return;
	}
	if (cv == obs_get_main_canvas()) {
		status->setText(QStringLiteral("主画布请用 OBS 自带推流按钮；这里推流用户画布"));
		return;
	}
	if (streams.contains(cv)) {
		status->setText(QStringLiteral("该画布已在推流"));
		return;
	}

	obs_service_t *cur = obs_frontend_get_streaming_service();
	if (!cur) {
		status->setText(QStringLiteral("尚未配置推流服务：请先在 设置 → 推流 填好服务器/串流密钥"));
		return;
	}
	const char *type = obs_service_get_type(cur);
	obs_data_t *sset = obs_service_get_settings(cur);
	if (!type || !sset) {
		if (sset) {
			obs_data_release(sset);
		}
		status->setText(QStringLiteral("读取推流服务设置失败"));
		return;
	}

	video_t *video = obs_canvas_get_video(cv);
	if (!video) {
		obs_data_release(sset);
		status->setText(QStringLiteral("该画布没有视频输出"));
		return;
	}

	const QString canvasName = QString::fromUtf8(obs_canvas_get_name(cv));
	const QString svcName = canvasName + QStringLiteral(" · 服务");
	obs_service_t *svc = obs_service_create(type, svcName.toUtf8().constData(), sset, nullptr);
	obs_data_release(sset);
	if (!svc) {
		status->setText(QStringLiteral("创建推流服务实例失败"));
		return;
	}

	QString encId = encCombo ? encCombo->currentData().toString() : QStringLiteral("obs_x264");
	QString usedEnc;
	obs_encoder_t *venc = CreateCanvasVideoEncoder(cv, encId, &usedEnc);
	if (!venc) {
		obs_service_release(svc);
		status->setText(QStringLiteral("创建视频编码器失败（无可用 H.264 编码器）"));
		return;
	}
	obs_encoder_set_video(venc, video);
	obs_encoder_t *aenc = CreateCanvasAudioEncoder(cv);

	const QString outName = canvasName + QStringLiteral(" · 推流");
	obs_output_t *out = obs_output_create("rtmp_output", outName.toUtf8().constData(), nullptr, nullptr);
	if (!out) {
		obs_encoder_release(venc);
		if (aenc) {
			obs_encoder_release(aenc);
		}
		obs_service_release(svc);
		status->setText(QStringLiteral("创建 RTMP 输出失败"));
		return;
	}
	obs_output_set_service(out, svc);
	obs_output_set_media(out, video, obs_get_audio());
	obs_output_set_video_encoder(out, venc);
	if (aenc) {
		obs_output_set_audio_encoder(out, aenc, 0);
	}

	if (!obs_output_start(out)) {
		const char *err = obs_output_get_last_error(out);
		status->setText(QStringLiteral("开始推流失败：%1").arg(err ? QString::fromUtf8(err) : QStringLiteral("未知错误")));
		obs_output_release(out);
		obs_encoder_release(venc);
		if (aenc) {
			obs_encoder_release(aenc);
		}
		obs_service_release(svc);
		return;
	}

	HfrStream st;
	st.svc = svc;
	st.out = out;
	st.venc = venc;
	st.aenc = aenc;
	streams.insert(cv, st);
	RefreshCanvasList();
	status->setText(QStringLiteral("推流中：%1（编码器 %2）").arg(canvasName).arg(usedEnc));
	HfrLog("canvas streaming started");
}

void HFRConsoleDock::StopStreamingSelected()
{
	obs_canvas_t *cv = CanvasFromRow(canvasList->currentRow());
	if (!cv || !streams.contains(cv)) {
		status->setText(QStringLiteral("该画布没有在推流"));
		return;
	}
	HfrStream st = streams.take(cv);
	if (st.out) {
		obs_output_stop(st.out);
		obs_output_release(st.out);
	}
	if (st.venc) {
		obs_encoder_release(st.venc);
	}
	if (st.aenc) {
		obs_encoder_release(st.aenc);
	}
	if (st.svc) {
		obs_service_release(st.svc);
	}
	RefreshCanvasList();
	status->setText(QStringLiteral("已停止推流：%1").arg(QString::fromUtf8(obs_canvas_get_name(cv))));
	HfrLog("canvas streaming stopped");
}

void HFRConsoleDock::StopAllStreams()
{
	const auto keys = streams.keys();
	for (obs_canvas_t *cv : keys) {
		HfrStream st = streams.take(cv);
		if (st.out) {
			obs_output_stop(st.out);
			obs_output_release(st.out);
		}
		if (st.venc) {
			obs_encoder_release(st.venc);
		}
		if (st.aenc) {
			obs_encoder_release(st.aenc);
		}
		if (st.svc) {
			obs_service_release(st.svc);
		}
	}
}

/* ---------------- PPT（本地 LibreOffice 转 PDF + pdfium 渲染，无桥接） ---------------- */

static HFRPptDocument &PptSelfTestDoc()
{
	static HFRPptDocument doc;
	return doc;
}

static void WriteBmp32File(const QString &path, uint32_t w, uint32_t h, const std::vector<uint8_t> &bgra)
{
	const uint32_t imgSize = w * 4 * h;
	const uint32_t fileSize = 54 + imgSize;
	QFile f(path);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		return;
	}
	uint8_t hdr[54] = {0};
	hdr[0] = 'B';
	hdr[1] = 'M';
	memcpy(hdr + 2, &fileSize, 4);
	uint32_t off = 54;
	memcpy(hdr + 10, &off, 4);
	uint32_t hdrSize = 40;
	memcpy(hdr + 14, &hdrSize, 4);
	memcpy(hdr + 18, &w, 4);
	memcpy(hdr + 22, &h, 4);
	uint16_t planes = 1, bpp = 32;
	memcpy(hdr + 26, &planes, 2);
	memcpy(hdr + 28, &bpp, 2);
	memcpy(hdr + 34, &imgSize, 4);
	f.write((const char *)hdr, 54);
	f.write((const char *)bgra.data(), (qint64)imgSize);
	f.close();
}

static void PptRenderAndReport(QLabel *status)
{
	HFRPptDocument &doc = PptSelfTestDoc();
	if (!doc.IsOpen()) {
		status->setText(QStringLiteral("PPT：尚未打开文件"));
		return;
	}
	std::vector<uint8_t> buf;
	QString err;
	if (!doc.RenderCurrentPart(1280, 720, buf, &err)) {
		status->setText(QStringLiteral("PPT 渲染失败：%1").arg(err));
		return;
	}
	const QString out = HfrPptTestOutputDir() + QStringLiteral("/hfr_ppt_preview.bmp");
	WriteBmp32File(out, 1280, 720, buf);

	size_t nonWhite = 0;
	const size_t px = (size_t)1280 * 720;
	for (size_t i = 0; i < px; i++) {
		const uint8_t b = buf[i * 4 + 0], g = buf[i * 4 + 1], r = buf[i * 4 + 2];
		if (!(b > 245 && g > 245 && r > 245)) {
			nonWhite++;
		}
	}
	const double pct = px ? (100.0 * (double)nonWhite / (double)px) : 0.0;
	status->setText(QStringLiteral("PPT 渲染成功：非白像素 %1%（1280x720，第 %2/%3 页）→ %4")
				.arg(pct, 0, 'f', 2)
				.arg(doc.CurrentPart() + 1)
				.arg(doc.PartCount())
				.arg(out));
}

void HFRConsoleDock::PptOpenTest()
{
	const QString file = QFileDialog::getOpenFileName(this, QStringLiteral("选择 PPT 文件"), QString(),
							  QStringLiteral("演示文稿 (*.pptx *.ppt *.odp);;所有文件 (*)"));
	if (file.isEmpty()) {
		return;
	}
	QString err;
	if (!PptSelfTestDoc().Open(file, &err)) {
		status->setText(QStringLiteral("PPT 打开失败：%1").arg(err));
		return;
	}
	PptRenderAndReport(status);
}

void HFRConsoleDock::PptNext()
{
	HFRPptDocument &doc = PptSelfTestDoc();
	if (!doc.IsOpen()) {
		status->setText(QStringLiteral("PPT：尚未打开文件"));
		return;
	}
	doc.NextPart();
	PptRenderAndReport(status);
}

void HFRConsoleDock::PptPrev()
{
	HFRPptDocument &doc = PptSelfTestDoc();
	if (!doc.IsOpen()) {
		status->setText(QStringLiteral("PPT：尚未打开文件"));
		return;
	}
	doc.PrevPart();
	PptRenderAndReport(status);
}

/* ---------------- 持久化 ---------------- */

QString HFRConsoleDock::StateFilePath() const
{
	char *path = obs_frontend_get_current_profile_path();
	QString dir = path ? QString::fromUtf8(path) : QString();
	if (path) {
		bfree(path);
	}
	if (dir.isEmpty()) {
		return QString();
	}
	return dir + QStringLiteral("/hfr_canvases.json");
}

void HFRConsoleDock::SaveState()
{
	if (loadingState) {
		return;
	}
	const QString file = StateFilePath();
	if (file.isEmpty()) {
		return;
	}

	QJsonArray canvasArr;
	for (obs_canvas_t *cv : canvases) {
		if (!cv) {
			continue;
		}
		struct obs_video_info ovi = {};
		obs_canvas_get_video_info(cv, &ovi);
		QJsonObject o;
		o[QStringLiteral("name")] = QString::fromUtf8(obs_canvas_get_name(cv));
		o[QStringLiteral("w")] = (int)ovi.base_width;
		o[QStringLiteral("h")] = (int)ovi.base_height;
		o[QStringLiteral("test")] = testSceneAdded.value(cv, false);
		o[QStringLiteral("safe")] = safeAreaPref.value(cv, false);

		QJsonArray scenesArr;
		std::vector<obs_source_t *> scenes;
		obs_canvas_enum_scenes(cv, CollectSceneSourcesCallback, &scenes);
		for (obs_source_t *s : scenes) {
			scenesArr.append(QString::fromUtf8(obs_source_get_name(s)));
		}
		o[QStringLiteral("scenes")] = scenesArr;

		obs_source_t *cur = obs_canvas_get_channel(cv, 0);
		o[QStringLiteral("cur")] = cur ? QString::fromUtf8(obs_source_get_name(cur)) : QString();
		canvasArr.append(o);
	}

	QJsonArray projArr;
	for (const HFRProjection &pr : projections) {
		if (!pr.window || !pr.canvas) {
			continue;
		}
		const auto screens = QGuiApplication::screens();
		QString screenName = (pr.screenIndex >= 0 && pr.screenIndex < screens.size())
					     ? screens.at(pr.screenIndex)->name()
					     : QString();
		QJsonObject o;
		o[QStringLiteral("canvas")] = QString::fromUtf8(obs_canvas_get_name(pr.canvas));
		o[QStringLiteral("screen")] = pr.screenIndex;
		o[QStringLiteral("screenName")] = screenName;
		o[QStringLiteral("safe")] = pr.window->HasSafeArea();
		projArr.append(o);
	}

	QJsonObject root;
	root[QStringLiteral("version")] = 1;
	root[QStringLiteral("canvases")] = canvasArr;
	root[QStringLiteral("projections")] = projArr;

	QFile f(file);
	if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
		f.close();
	}
}

void HFRConsoleDock::LoadState()
{
	const QString file = StateFilePath();
	if (file.isEmpty() || !QFile::exists(file)) {
		return;
	}
	QFile f(file);
	if (!f.open(QIODevice::ReadOnly)) {
		return;
	}
	const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
	f.close();
	if (!doc.isObject()) {
		return;
	}

	loadingState = true;

	const QJsonArray canvasArr = doc.object().value(QStringLiteral("canvases")).toArray();
	for (const QJsonValue &v : canvasArr) {
		const QJsonObject o = v.toObject();
		const QString name = o.value(QStringLiteral("name")).toString();
		const uint32_t w = (uint32_t)o.value(QStringLiteral("w")).toInt(1280);
		const uint32_t h = (uint32_t)o.value(QStringLiteral("h")).toInt(720);
		if (name.isEmpty()) {
			continue;
		}
		struct obs_video_info ovi = {};
		obs_get_video_info(&ovi);
		ovi.base_width = w ? w : 1280;
		ovi.base_height = h ? h : 720;
		obs_canvas_t *cv = obs_canvas_create(name.toUtf8().constData(), &ovi, ACTIVATE | SCENE_REF);
		if (!cv) {
			continue;
		}
		canvases.append(cv);
		safeAreaPref.insert(cv, o.value(QStringLiteral("safe")).toBool(false));

		const bool isTest = o.value(QStringLiteral("test")).toBool(false);
		const QString curName = o.value(QStringLiteral("cur")).toString();
		const QJsonArray scenesArr = o.value(QStringLiteral("scenes")).toArray();

		if (scenesArr.isEmpty() && isTest) {
			/* 兼容旧存档：只有 test 标记 */
			SetCanvasTestColor(cv);
			testSceneAdded.insert(cv, true);
		} else {
			for (const QJsonValue &sv : scenesArr) {
				const QString sname = sv.toString();
				if (sname.isEmpty()) {
					continue;
				}
				obs_scene_t *sc = obs_canvas_scene_create(cv, sname.toUtf8().constData());
				if (!sc) {
					continue;
				}
				/* 测试色场景：重建其色块内容 */
				if (isTest && sname.endsWith(QStringLiteral(" · 测试色块"))) {
					AddColorIntoScene(cv, sc, sname.left(sname.length() - QStringLiteral("块").length()) + QStringLiteral("色"));
					testSceneAdded.insert(cv, true);
				}
				if (!curName.isEmpty() && sname == curName) {
					obs_canvas_set_channel(cv, 0, obs_scene_get_source(sc));
				}
			}
		}
	}

	RefreshCanvasList();

	const QJsonArray projArr = doc.object().value(QStringLiteral("projections")).toArray();
	const auto screens = QGuiApplication::screens();
	for (const QJsonValue &v : projArr) {
		const QJsonObject o = v.toObject();
		const QString cname = o.value(QStringLiteral("canvas")).toString();
		int idx = o.value(QStringLiteral("screen")).toInt(-1);
		const QString savedScreenName = o.value(QStringLiteral("screenName")).toString();
		const bool safe = o.value(QStringLiteral("safe")).toBool(false);

		obs_canvas_t *cv = nullptr;
		if (cname == QStringLiteral("Main") || cname.isEmpty()) {
			cv = obs_get_main_canvas();
		} else {
			for (obs_canvas_t *c : canvases) {
				if (c && cname == QString::fromUtf8(obs_canvas_get_name(c))) {
					cv = c;
					break;
				}
			}
		}
		if (!cv) {
			continue;
		}
		/* 显示器顺序可能变化：优先按名称匹配，其次按索引 */
		if (!savedScreenName.isEmpty()) {
			for (int i = 0; i < screens.size(); i++) {
				if (screens.at(i)->name() == savedScreenName) {
					idx = i;
					break;
				}
			}
		}
		if (idx < 0 || idx >= screens.size()) {
			continue;
		}
		ProjectCanvasToMonitor(cv, idx);
		if (!projections.isEmpty()) {
			projections.last().window->SetSafeArea(safe);
		}
	}

	loadingState = false;
	HfrLog("state loaded");
}

void HFRConsoleDock::RemoveSelectedCanvas()
{
	int row = canvasList->currentRow();
	obs_canvas_t *cv = CanvasFromRow(row);
	if (!cv) {
		return;
	}
	if (cv == obs_get_main_canvas()) {
		status->setText(QStringLiteral("主画布不可删除"));
		return;
	}
	if (!canvases.contains(cv)) {
		return;
	}
	for (int i = projections.size() - 1; i >= 0; i--) {
		if (projections[i].canvas == cv) {
			if (projections[i].window) {
				projections[i].window->close();
			}
			projections.removeAt(i);
		}
	}
	canvases.removeAll(cv);
	testSceneAdded.remove(cv);
	safeAreaPref.remove(cv);
	TeardownUserCanvas(cv);
	RefreshCanvasList();
	SaveState();
	HfrLog("removed canvas");
	status->setText(QStringLiteral("已删除画布"));
}

void HFRConsoleDock::SetCanvasTestColor(obs_canvas_t *cv)
{
	if (!cv || cv == obs_get_main_canvas()) {
		status->setText(QStringLiteral("主画布由主界面管理，测试色仅用于用户画布"));
		return;
	}
	/* 已填充检测：直接看通道 0 是否已是本画布的测试场景（比内存标记更可靠） */
	const QByteArray canvasName = obs_canvas_get_name(cv);
	const std::string testSceneName = canvasName.toStdString() + " · 测试色块";
	if (obs_source_t *cur = obs_canvas_get_channel(cv, 0)) {
		const char *curName = obs_source_get_name(cur);
		if (curName && testSceneName == curName) {
			status->setText(QStringLiteral("该画布已填充测试色"));
			return;
		}
	}

	/* 名称按画布唯一化，避免多画布重名导致场景创建失败（表现为黑屏） */
	obs_scene_t *sc = obs_canvas_scene_create(cv, testSceneName.c_str());
	if (!sc) {
		status->setText(QStringLiteral("创建测试场景失败（名称冲突？）"));
		HfrLog("test scene create FAILED");
		return;
	}

	/* 色块尺寸 = 该画布基础分辨率，并拉伸铺满，任何分辨率/宽高比都正确 */
	struct obs_video_info ovi = {};
	uint32_t w = 1280, h = 720;
	if (obs_canvas_get_video_info(cv, &ovi) && ovi.base_width && ovi.base_height) {
		w = ovi.base_width;
		h = ovi.base_height;
	}

	AddColorIntoScene(cv, sc, canvasName + QStringLiteral(" · 测试色"));

	obs_canvas_set_channel(cv, 0, obs_scene_get_source(sc));
	testSceneAdded.insert(cv, true);
	HfrLog("test color scene added to canvas (sized to canvas)");
	status->setText(QStringLiteral("已填充测试色（%1x%2 铺满）").arg(w).arg(h));
}

void HFRConsoleDock::SetTestColorOnSelected()
{
	obs_canvas_t *cv = CanvasFromRow(canvasList->currentRow());
	if (!cv) {
		status->setText(QStringLiteral("请先点选画布"));
		return;
	}
	SetCanvasTestColor(cv);
	status->setText(QStringLiteral("已填充测试色：%1").arg(QString::fromUtf8(obs_canvas_get_name(cv))));
}

void HFRConsoleDock::ProjectCanvasToMonitor(obs_canvas_t *cv, int mRow)
{
	const auto screens = QGuiApplication::screens();
	if (!cv || mRow < 0 || mRow >= screens.size()) {
		status->setText(QStringLiteral("画布或显示器编号无效"));
		return;
	}
	/* 同画布+同屏已在投 → 前置即可 */
	for (HFRProjection &pr : projections) {
		if (pr.window && pr.canvas == cv && pr.screenIndex == mRow) {
			pr.window->raise();
			pr.window->activateWindow();
			status->setText(QStringLiteral("已在前台：%1 → 屏%2")
						.arg(QString::fromUtf8(obs_canvas_get_name(cv)))
						.arg(mRow));
			return;
		}
	}
	HFRProjection pr;
	pr.window = new HFRCanvasProjector(cv, screens.at(mRow), nullptr);
	pr.window->SetSafeArea(safeAreaPref.value(cv, false));
	pr.canvas = cv;
	pr.screenIndex = mRow;
	projections.append(pr);
	SaveState();
	HfrLog("projected canvas to screen");
	status->setText(QStringLiteral("已投影 %1 → 屏%2（投影 %3 个）")
				.arg(QString::fromUtf8(obs_canvas_get_name(cv)))
				.arg(mRow)
				.arg(projections.size()));
}

void HFRConsoleDock::ProjectSelected()
{
	int cRow = canvasList->currentRow();
	int mRow = monitorList->currentRow();
	obs_canvas_t *cv = CanvasFromRow(cRow);
	if (!cv) {
		status->setText(QStringLiteral("请先点选画布"));
		return;
	}
	ProjectCanvasToMonitor(cv, mRow);
}

void HFRConsoleDock::ToggleSafeAreaOnSelected()
{
	obs_canvas_t *cv = CanvasFromRow(canvasList->currentRow());
	if (!cv) {
		status->setText(QStringLiteral("请先点选画布"));
		return;
	}
	bool on = !safeAreaPref.value(cv, false);
	safeAreaPref.insert(cv, on);
	int n = 0;
	for (HFRProjection &pr : projections) {
		if (pr.canvas == cv && pr.window) {
			pr.window->SetSafeArea(on);
			n++;
		}
	}
	status->setText(QStringLiteral("%1 安全区：%2（作用于 %3 个投影；新投影自动沿用）")
				.arg(QString::fromUtf8(obs_canvas_get_name(cv)))
				.arg(on ? QStringLiteral("开") : QStringLiteral("关"))
				.arg(n));
	SaveState();
	HfrLog(on ? "safe area ON" : "safe area OFF");
}

void HFRConsoleDock::StopProjectionOfSelected()
{
	obs_canvas_t *cv = CanvasFromRow(canvasList->currentRow());
	if (!cv) {
		status->setText(QStringLiteral("请先点选画布"));
		return;
	}
	int before = projections.size();
	for (int i = projections.size() - 1; i >= 0; i--) {
		HFRProjection &pr = projections[i];
		if (pr.canvas == cv) {
			if (pr.window) {
				pr.window->close();
			}
			projections.removeAt(i);
		}
	}
	status->setText(QStringLiteral("已停止 %1 的投影（%2 → %3 个）")
				.arg(QString::fromUtf8(obs_canvas_get_name(cv)))
				.arg(before - projections.size())
				.arg(projections.size()));
	SaveState();
}

void HFRConsoleDock::CloseAllProjectors()
{
	for (HFRProjection &pr : projections) {
		if (pr.window) {
			pr.window->close();
		}
	}
	projections.clear();
	SaveState();
}