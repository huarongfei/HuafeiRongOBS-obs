#include "HFRPptPlayer.hpp"
#include "HFRStrings.hpp"

#include "HFRPresenter.hpp"
#include "HFRPptThumbnails.hpp"
#include "HFRSlidesSource.hpp"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QFileDialog>
#include <QDateTime>
#include <QColor>
#include <QFileInfo>
#include <QFont>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QAbstractItemModel>
#include <QListWidget>
#include <QPushButton>
#include <QScreen>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <obs-module.h>

#include <algorithm>
#include <cstring>

/* ============================ 播放窗 ============================ */

HFRPptPlayerWindow::HFRPptPlayerWindow(QWidget *parent) : OBSQTDisplay(parent)
{
	setWindowTitle(hfrt::T("k48111", "PPT 独立播放", "HFR PPT Player"));
	setAttribute(Qt::WA_QuitOnClose, false);
	setFocusPolicy(Qt::StrongFocus);
	setStyleSheet(QStringLiteral("background:#000;"));

	auto addDraw = [this]() {
		obs_display_add_draw_callback(GetDisplay(), &HFRPptPlayerWindow::Draw, this);
		obs_display_set_background_color(GetDisplay(), 0x000000);
	};
	connect(this, &OBSQTDisplay::DisplayCreated, this, addDraw);

	/* 备注浮层：定时检查当前页备注变化并排版（UI 线程，避免占用图形线程） */
	notesTimer = new QTimer(this);
	notesTimer->setInterval(300);
	connect(notesTimer, &QTimer::timeout, this, [this]() { UpdateNotesSprite(); });
	notesTimer->start();
}

HFRPptPlayerWindow::~HFRPptPlayerWindow()
{
	if (GetDisplay()) {
		obs_display_remove_draw_callback(GetDisplay(), &HFRPptPlayerWindow::Draw, this);
	}
	if (notesTex) {
		obs_enter_graphics();
		gs_texture_destroy(notesTex);
		obs_leave_graphics();
		notesTex = nullptr;
	}
	if (src) {
		obs_source_release(src);
		src = nullptr;
	}
}

void HFRPptPlayerWindow::CreateInternalSource()
{
	if (src) {
		return;
	}
	obs_data_t *s = obs_data_create();
	obs_data_set_int(s, "width", 1920);
	obs_data_set_int(s, "height", 1080);
	src = obs_source_create("hfr_slides_source", "HFR-PPT-Player", s, nullptr);
	obs_data_release(s);
}

bool HFRPptPlayerWindow::OpenFile(const QString &path, QString *err)
{
	if (path.isEmpty()) {
		return false;
	}
	CreateInternalSource();
	if (!src) {
		if (err) {
			*err = QStringLiteral("无法创建内部幻灯片来源（来源类型未注册？）");
		}
		return false;
	}
	HfrSlidesSetFile(src, path);
	filePath = path;
	black = false;
	setWindowTitle(QStringLiteral("PPT 独立播放 · %1").arg(QFileInfo(path).fileName()));
	return true;
}

void HFRPptPlayerWindow::ShowOnScreen(int screenIndex)
{
	const auto screens = QGuiApplication::screens();
	if (screens.isEmpty()) {
		return;
	}
	const int idx = (screenIndex >= 0 && screenIndex < screens.size()) ? screenIndex : 0;
	setGeometry(screens.at(idx)->geometry());
	if (fullscreen) {
		showFullScreen();
	} else {
		show();
	}
	raise();
	activateWindow();
	setFocus(Qt::OtherFocusReason);
}

void HFRPptPlayerWindow::ClosePlayer()
{
	hide();
}

void HFRPptPlayerWindow::SetBlack(bool on)
{
	black = on;
	update();
}

void HFRPptPlayerWindow::SetShowNotes(bool on)
{
	showNotes = on;
	notesCache.clear();
	notesDirty = true;
	UpdateNotesSprite();
	update();
}

void HFRPptPlayerWindow::UpdateNotesSprite()
{
	if (!showNotes || !src) {
		return;
	}
	const std::wstring wn = HfrSlidesNotes(src);
	const QString notes = wn.empty() ? QString() : QString::fromWCharArray(wn.c_str());
	if (notes == notesCache) {
		return;
	}
	notesCache = notes;

	const int w = 1600;
	const int h = 420;
	QImage img(w, h, QImage::Format_ARGB32);
	img.fill(Qt::transparent);
	{
		QPainter p(&img);
		p.fillRect(0, 0, w, h, QColor(0, 0, 0, 165));
		p.setPen(Qt::white);
		QFont f(QStringLiteral("Microsoft YaHei"));
		f.setPixelSize(30);
		p.setFont(f);
		p.drawText(QRect(44, 24, w - 88, h - 48), Qt::TextWordWrap | Qt::AlignTop,
			   notes.isEmpty() ? QStringLiteral("（本页无备注）") : notes);
		p.end();
	}
	const int bytes = w * h * 4;
	notesPix.resize((size_t)bytes);
	const uchar *bits = img.constBits();
	memcpy(notesPix.data(), bits, (size_t)bytes);
	notesPixW = w;
	notesPixH = h;
	notesDirty = true;
}

void HFRPptPlayerWindow::DrawNotesOverlay(uint32_t cx, uint32_t cy)
{
	if (!showNotes || !cx || !cy) {
		return;
	}
	if (notesDirty) {
		if (notesTex) {
			obs_enter_graphics();
			gs_texture_destroy(notesTex);
			obs_leave_graphics();
			notesTex = nullptr;
		}
		if (!notesPix.empty() && notesPixW > 0 && notesPixH > 0) {
			obs_enter_graphics();
			const uint8_t *ptr = notesPix.data();
			notesTex = gs_texture_create((uint32_t)notesPixW, (uint32_t)notesPixH, GS_BGRA, 1, &ptr,
						     GS_DYNAMIC);
			obs_leave_graphics();
		}
		notesDirty = false;
	}
	if (!notesTex) {
		return;
	}
	const float dh = (float)cy * 0.42f;
	const float dw = dh * ((float)notesPixW / (float)notesPixH);
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), notesTex);
	gs_viewport_push();
	gs_projection_push();
	gs_set_viewport(0, 0, (int)cx, (int)cy);
	gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
	gs_matrix_push();
	gs_matrix_translate3f(((float)cx - dw) * 0.5f, 0.0f, 0.0f);
	while (gs_effect_loop(effect, "Draw")) {
		gs_draw_sprite(notesTex, 0, (uint32_t)dw, (uint32_t)dh);
	}
	gs_matrix_pop();
	gs_projection_pop();
	gs_viewport_pop();
}

int HFRPptPlayerWindow::Page() const
{
	return src ? HfrSlidesPage(src) : 0;
}

int HFRPptPlayerWindow::PageCount() const
{
	return src ? HfrSlidesPageCount(src) : 0;
}

int HFRPptPlayerWindow::AnimStep() const
{
	return src ? HfrSlidesAnimStep(src) : 0;
}

int HFRPptPlayerWindow::AnimStepCount() const
{
	return src ? HfrSlidesAnimStepCount(src) : 0;
}

bool HFRPptPlayerWindow::AtEnd() const
{
	if (!src) {
		return true;
	}
	const int total = HfrSlidesPageCount(src);
	const int cur = HfrSlidesPage(src);
	return (cur + 1 >= total) && (HfrSlidesAnimStep(src) >= HfrSlidesAnimStepCount(src));
}

void HFRPptPlayerWindow::Next()
{
	if (!src) {
		return;
	}
	if (HfrSlidesNextAnim(src)) {
		return;
	}
	const int total = HfrSlidesPageCount(src);
	const int cur = HfrSlidesPage(src);
	if (cur + 1 < total) {
		HfrSlidesSetPage(src, cur + 1);
		return;
	}
	/* 已在最后：通知连播队列 */
	if (onEndReached) {
		onEndReached();
	}
}

void HFRPptPlayerWindow::Prev()
{
	if (!src) {
		return;
	}
	if (HfrSlidesPrevAnim(src)) {
		return;
	}
	const int cur = HfrSlidesPage(src);
	if (cur > 0) {
		HfrSlidesSetPage(src, cur - 1);
	}
}

void HFRPptPlayerWindow::NextAnim()
{
	if (src) {
		HfrSlidesNextAnim(src);
	}
}

void HFRPptPlayerWindow::PrevAnim()
{
	if (src) {
		HfrSlidesPrevAnim(src);
	}
}

void HFRPptPlayerWindow::FirstPage()
{
	if (src) {
		HfrSlidesSetPage(src, 0);
	}
}

void HFRPptPlayerWindow::LastPage()
{
	if (src) {
		const int total = HfrSlidesPageCount(src);
		if (total > 0) {
			HfrSlidesSetPage(src, total - 1);
		}
	}
}

void HFRPptPlayerWindow::SetPage(int page)
{
	if (src) {
		HfrSlidesSetPage(src, page);
	}
}

void HFRPptPlayerWindow::keyPressEvent(QKeyEvent *event)
{
	switch (event->key()) {
	case Qt::Key_Right:
	case Qt::Key_Down:
	case Qt::Key_PageDown:
	case Qt::Key_Space:
	case Qt::Key_Return:
	case Qt::Key_Enter:
		Next();
		return;
	case Qt::Key_Left:
	case Qt::Key_Up:
	case Qt::Key_PageUp:
	case Qt::Key_Backspace:
		Prev();
		return;
	case Qt::Key_Home:
		FirstPage();
		return;
	case Qt::Key_End:
		LastPage();
		return;
	case Qt::Key_B:
		SetBlack(!black);
		return;
	case Qt::Key_N:
		SetShowNotes(!showNotes);
		return;
	case Qt::Key_Escape:
		ClosePlayer();
		return;
	default:
		break;
	}
	QWidget::keyPressEvent(event);
}

void HFRPptPlayerWindow::closeEvent(QCloseEvent *event)
{
	/* Esc/关闭 = 隐藏播放窗，保留来源与页码，方便随时重新上台 */
	event->accept();
	hide();
}

void HFRPptPlayerWindow::Draw(void *data, uint32_t cx, uint32_t cy)
{
	HFRPptPlayerWindow *w = static_cast<HFRPptPlayerWindow *>(data);
	if (!w || !w->src || w->black || !cx || !cy) {
		return;
	}
	const uint32_t sw = obs_source_get_width(w->src);
	const uint32_t sh = obs_source_get_height(w->src);
	if (!sw || !sh) {
		return;
	}
	const float scale = std::min((float)cx / (float)sw, (float)cy / (float)sh);
	uint32_t nw = (uint32_t)(scale * (float)sw);
	uint32_t nh = (uint32_t)(scale * (float)sh);
	if (!nw || !nh) {
		return;
	}
	gs_viewport_push();
	gs_projection_push();
	gs_set_viewport((int)((cx - nw) / 2), (int)((cy - nh) / 2), (int)nw, (int)nh);
	gs_ortho(0.0f, (float)sw, 0.0f, (float)sh, -100.0f, 100.0f);
	obs_source_video_render(w->src);
	gs_projection_pop();
	gs_viewport_pop();

	/* 备注浮层（叠加在画面底部） */
	w->DrawNotesOverlay(cx, cy);
}

/* ============================ PVW 预览窗 ============================ */

HFRPptPreviewWindow::HFRPptPreviewWindow(QWidget *parent) : OBSQTDisplay(parent)
{
	setWindowTitle(QStringLiteral("PPT 预览（下一页）"));
	setAttribute(Qt::WA_QuitOnClose, false);
	setFocusPolicy(Qt::NoFocus);
	setStyleSheet(QStringLiteral("background:#000;"));
	auto addDraw = [this]() {
		obs_display_add_draw_callback(GetDisplay(), &HFRPptPreviewWindow::Draw, this);
		obs_display_set_background_color(GetDisplay(), 0x000000);
	};
	connect(this, &OBSQTDisplay::DisplayCreated, this, addDraw);
}

HFRPptPreviewWindow::~HFRPptPreviewWindow()
{
	if (GetDisplay()) {
		obs_display_remove_draw_callback(GetDisplay(), &HFRPptPreviewWindow::Draw, this);
	}
	if (tex) {
		obs_enter_graphics();
		gs_texture_destroy(tex);
		obs_leave_graphics();
		tex = nullptr;
	}
}

void HFRPptPreviewWindow::ShowOnScreen(int screenIndex)
{
	const auto screens = QGuiApplication::screens();
	if (screens.isEmpty()) {
		return;
	}
	const int idx = (screenIndex >= 0 && screenIndex < screens.size()) ? screenIndex : 0;
	setGeometry(screens.at(idx)->geometry());
	showFullScreen();
	raise();
}

void HFRPptPreviewWindow::ClosePreview()
{
	hide();
}

void HFRPptPreviewWindow::SetImage(const std::vector<uint8_t> &bgra, int w, int h)
{
	if (w <= 0 || h <= 0 || bgra.empty()) {
		return;
	}
	pix = bgra;
	pw = w;
	ph = h;
	dirty = true;
	update();
}

void HFRPptPreviewWindow::closeEvent(QCloseEvent *event)
{
	event->accept();
	hide();
}

void HFRPptPreviewWindow::Draw(void *data, uint32_t cx, uint32_t cy)
{
	HFRPptPreviewWindow *w = static_cast<HFRPptPreviewWindow *>(data);
	if (!w || !cx || !cy) {
		return;
	}
	if (w->dirty) {
		if (w->tex) {
			obs_enter_graphics();
			gs_texture_destroy(w->tex);
			obs_leave_graphics();
			w->tex = nullptr;
		}
		if (!w->pix.empty() && w->pw > 0 && w->ph > 0) {
			obs_enter_graphics();
			const uint8_t *ptr = w->pix.data();
			w->tex = gs_texture_create((uint32_t)w->pw, (uint32_t)w->ph, GS_BGRA, 1, &ptr, 0);
			obs_leave_graphics();
		}
		w->dirty = false;
	}
	if (!w->tex) {
		return;
	}
	const float scale = std::min((float)cx / (float)w->pw, (float)cy / (float)w->ph);
	uint32_t nw = (uint32_t)(scale * (float)w->pw);
	uint32_t nh = (uint32_t)(scale * (float)w->ph);
	if (!nw || !nh) {
		return;
	}
	gs_viewport_push();
	gs_projection_push();
	gs_set_viewport((int)((cx - nw) / 2), (int)((cy - nh) / 2), (int)nw, (int)nh);
	gs_ortho(0.0f, (float)w->pw, 0.0f, (float)w->ph, -100.0f, 100.0f);
	gs_effect_t *eff = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_effect_set_texture(gs_effect_get_param_by_name(eff, "image"), w->tex);
	while (gs_effect_loop(eff, "Draw")) {
		gs_draw_sprite(w->tex, 0, (uint32_t)w->pw, (uint32_t)w->ph);
	}
	gs_projection_pop();
	gs_viewport_pop();
}

/* ============================ 控制面板 ============================ */

HFRPptPlayerControl::HFRPptPlayerControl(QWidget *parent) : QWidget(parent)
{
	setWindowTitle(hfrt::T("k56595", "PPT 独立播放（控制台）", "HFR PPT Player (Console)"));
	setWindowFlags(Qt::Tool | Qt::WindowStaysOnTopHint);
	resize(560, 210);

	QVBoxLayout *root = new QVBoxLayout(this);
	root->setContentsMargins(8, 8, 8, 8);
	root->setSpacing(6);

	fileLabel = new QLabel(hfrt::T("k65677", "未打开演示文稿", "No presentation"), this);
	fileLabel->setStyleSheet(QStringLiteral("font-weight:bold;"));
	root->addWidget(fileLabel);

	/* 播放队列（多文件连播） */
	QLabel *queueTitle = new QLabel(QStringLiteral("播放队列（勾选右侧「连续播放」后自动切下一个文件）"), this);
	queueTitle->setStyleSheet(QStringLiteral("color:#555; font-size:11px;"));
	root->addWidget(queueTitle);
	queueList = new QListWidget(this);
	queueList->setMaximumHeight(84);
	queueList->setDragDropMode(QAbstractItemView::InternalMove);
	queueList->setDefaultDropAction(Qt::MoveAction);
	queueList->setToolTip(QStringLiteral("可拖拽调整播放顺序"));
	root->addWidget(queueList);
	QHBoxLayout *queueBtns = new QHBoxLayout();
	btnQueueAdd = new QPushButton(QStringLiteral("+ 添加文件"), this);
	btnQueueDel = new QPushButton(QStringLiteral("移除"), this);
	btnQueueUp = new QPushButton(QStringLiteral("上移"), this);
	btnQueueDown = new QPushButton(QStringLiteral("下移"), this);
	chkQueueAuto = new QCheckBox(hfrt::T("k20952", "连续播放", "Auto advance"), this);
	chkQueueLoop = new QCheckBox(hfrt::T("k44416", "循环", "Loop"), this);
	chkQueueLoop->setToolTip(QStringLiteral("队列播完后从头循环（展会/展台常用）"));
	queueLabel = new QLabel(QStringLiteral("队列 0/0"), this);
	queueLabel->setStyleSheet(QStringLiteral("color:#555; font-size:11px;"));
	queueBtns->addWidget(btnQueueAdd);
	queueBtns->addWidget(btnQueueDel);
	queueBtns->addWidget(btnQueueUp);
	queueBtns->addWidget(btnQueueDown);
	queueBtns->addStretch(1);
	queueBtns->addWidget(queueLabel);
	queueBtns->addWidget(chkQueueAuto);
	queueBtns->addWidget(chkQueueLoop);
	root->addLayout(queueBtns);

	connect(btnQueueAdd, &QPushButton::clicked, this, [this]() {
		const QStringList files = QFileDialog::getOpenFileNames(
			this, QStringLiteral("添加到播放队列"), QString(),
			QStringLiteral("演示文稿 (*.pptx *.ppt *.odp)"));
		for (const QString &path : files) {
			queueFiles.push_back(path);
			queueList->addItem(QFileInfo(path).fileName());
		}
		UpdateQueueLabel();
	});
	connect(btnQueueDel, &QPushButton::clicked, this, [this]() {
		const int row = queueList->currentRow();
		if (row >= 0 && row < queueFiles.size()) {
			queueFiles.removeAt(row);
			delete queueList->takeItem(row);
			if (queueIndex > row) {
				queueIndex--;
			} else if (queueIndex == row) {
				queueIndex = -1;
			}
			UpdateQueueLabel();
		}
	});
	connect(btnQueueUp, &QPushButton::clicked, this, [this]() {
		const int row = queueList->currentRow();
		if (row > 0 && row < queueFiles.size()) {
			queueFiles.swapItemsAt(row, row - 1);
			QListWidgetItem *item = queueList->takeItem(row);
			queueList->insertItem(row - 1, item);
			queueList->setCurrentRow(row - 1);
		}
	});
	connect(btnQueueDown, &QPushButton::clicked, this, [this]() {
		const int row = queueList->currentRow();
		if (row >= 0 && row + 1 < queueFiles.size()) {
			queueFiles.swapItemsAt(row, row + 1);
			QListWidgetItem *item = queueList->takeItem(row);
			queueList->insertItem(row + 1, item);
			queueList->setCurrentRow(row + 1);
		}
	});
	connect(queueList->model(), &QAbstractItemModel::rowsMoved, this,
		[this](const QModelIndex &, int start, int end, const QModelIndex &, int destRow) {
			if (start < 0 || end < start || end >= queueFiles.size()) {
				return;
			}
			const int count = end - start + 1;
			const int curBefore = queueIndex;
			QVector<QString> moved;
			for (int i = 0; i < count; i++) {
				moved.push_back(queueFiles.takeAt(start));
			}
			int insertAt = destRow;
			if (destRow > start) {
				insertAt -= count;
			}
			insertAt = qBound(0, insertAt, queueFiles.size());
			for (int i = 0; i < moved.size(); i++) {
				queueFiles.insert(insertAt + i, moved.at(i));
			}
			if (curBefore >= 0) {
				if (curBefore >= start && curBefore <= end) {
					queueIndex = insertAt + (curBefore - start);
				} else if (destRow > start) {
					if (curBefore > end && curBefore < destRow) {
						queueIndex = curBefore - count;
					}
				} else if (curBefore >= destRow && curBefore < start) {
					queueIndex = curBefore + count;
				}
			}
			UpdateQueueLabel();
		});
	connect(chkQueueAuto, &QCheckBox::toggled, this, [this](bool on) {
		if (!player) {
			return;
		}
		if (on) {
			player->SetEndReachedCallback([this]() { AdvanceQueue(); });
		} else {
			player->SetEndReachedCallback(nullptr);
		}
	});

	QHBoxLayout *row1 = new QHBoxLayout();
	btnOpen = new QPushButton(hfrt::T("k70279", "打开 PPT…", "Open PPT..."), this);
	btnPrev = new QPushButton(hfrt::T("k59391", "◀ 上一页", "Prev"), this);
	btnNext = new QPushButton(hfrt::T("k54939", "下一页 ▶", "Next"), this);
	btnPrevAnim = new QPushButton(hfrt::T("k12072", "◀ 动画", "Anim prev"), this);
	btnNextAnim = new QPushButton(hfrt::T("k34950", "动画 ▶", "Anim next"), this);
	row1->addWidget(btnOpen);
	row1->addWidget(btnPrev);
	row1->addWidget(btnNext);
	row1->addWidget(btnPrevAnim);
	row1->addWidget(btnNextAnim);
	root->addLayout(row1);

	QHBoxLayout *row2 = new QHBoxLayout();
	pageLabel = new QLabel(hfrt::T("k44165", "第 -/- 页", "-/-"), this);
	screenCombo = new QComboBox(this);
	const auto screens = QGuiApplication::screens();
	for (int i = 0; i < screens.size(); i++) {
		screenCombo->addItem(QStringLiteral("屏%1 · %2x%3")
					     .arg(i)
					     .arg(screens.at(i)->geometry().width())
					     .arg(screens.at(i)->geometry().height()),
				     i);
	}
	btnShow = new QPushButton(hfrt::T("k69420", "全屏播放", "Fullscreen"), this);
	btnBlack = new QPushButton(hfrt::T("k25152", "黑屏(B)", "Black (B)"), this);
	btnPresenter = new QPushButton(hfrt::T("k31680", "讲者视图", "Presenter"), this);
	btnThumbs = new QPushButton(hfrt::T("k51615", "缩略图总览", "Thumbnails"), this);
	previewScreenCombo = new QComboBox(this);
	for (int i = 0; i < screens.size(); i++) {
		previewScreenCombo->addItem(QStringLiteral("预览屏%1 · %2x%3")
						    .arg(i)
						    .arg(screens.at(i)->geometry().width())
						    .arg(screens.at(i)->geometry().height()),
					    i);
	}
	btnPvw = new QPushButton(hfrt::T("k79435", "预览窗(下一页)", "PVW (next)"), this);
	btnPvw->setCheckable(true);
	btnPvw->setToolTip(QStringLiteral("在所选屏幕全屏显示下一页（PVW），用于操作员预览下一步"));
	btnNotes = new QPushButton(hfrt::T("k10025", "备注(N)", "Notes (N)"), this);
	btnNotes->setToolTip(QStringLiteral("在播放窗底部叠加当前页备注（也可按 N）"));
	pageJump = new QSpinBox(this);
	pageJump->setRange(1, 9999);
	pageJump->setToolTip(QStringLiteral("跳页：输入页码后点「跳转」"));
	btnJump = new QPushButton(hfrt::T("k53069", "跳转", "Go"), this);
	row2->addWidget(pageLabel);
	row2->addSpacing(8);
	row2->addWidget(pageJump);
	row2->addWidget(btnJump);
	row2->addSpacing(12);
	row2->addWidget(new QLabel(hfrt::T("k50116", "屏幕：", "Screen:"), this));
	row2->addWidget(screenCombo);
	row2->addWidget(btnShow);
	row2->addWidget(btnBlack);
	row2->addWidget(btnPresenter);
	row2->addWidget(btnThumbs);
	row2->addWidget(btnNotes);
	row2->addWidget(new QLabel(hfrt::T("k89268", "预览屏：", "PVW screen:"), this));
	row2->addWidget(previewScreenCombo);
	row2->addWidget(btnPvw);
	root->addLayout(row2);

	QHBoxLayout *row3 = new QHBoxLayout();
	autoPlay = new QCheckBox(hfrt::T("k24217", "自动播放", "Auto play"), this);
	autoSec = new QSpinBox(this);
	autoSec->setRange(1, 3600);
	autoSec->setValue(10);
	autoSec->setSuffix(QStringLiteral(" 秒"));
	row3->addWidget(autoPlay);
	row3->addWidget(autoSec);
	row3->addSpacing(16);
	timerLabel = new QLabel(QStringLiteral("用时 00:00"), this);
	timerLabel->setStyleSheet(QStringLiteral("color:#555;"));
	row3->addWidget(timerLabel);
	row3->addStretch(1);
	root->addLayout(row3);

	QLabel *hint = new QLabel(hfrt::T("hint.playerkeys",
					 "键盘：←/→/PageUp/PageDown/空格 翻页与动画 · B 黑屏 · Esc 退出全屏",
					 "Keys: Left/Right/PageUp/PageDown/Space page & animation - B black - Esc exit fullscreen"),
				  this);
	hint->setStyleSheet(QStringLiteral("color:#666; font-size:11px;"));
	hint->setWordWrap(true);
	root->addWidget(hint);

	player = new HFRPptPlayerWindow(nullptr);

	autoTimer = new QTimer(this);
	autoTimer->setInterval(autoSec->value() * 1000);
	connect(autoTimer, &QTimer::timeout, this, [this]() {
		if (player) {
			player->Next();
		}
	});
	connect(autoPlay, &QCheckBox::toggled, this, [this](bool on) {
		if (on) {
			autoTimer->start();
		} else {
			autoTimer->stop();
		}
	});
	connect(autoSec, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int sec) {
		autoTimer->setInterval(sec * 1000);
	});

	connect(btnOpen, &QPushButton::clicked, this, [this]() {
		const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("打开演示文稿"),
								  QString(), QStringLiteral("演示文稿 (*.pptx *.ppt *.odp)"));
		if (path.isEmpty() || !player) {
			return;
		}
		QString err;
		if (!player->OpenFile(path, &err)) {
			fileLabel->setText(QStringLiteral("打开失败：") + err);
			return;
		}
		fileLabel->setText(QStringLiteral("文件：") + QFileInfo(path).fileName());
		queueIndex = -1;
		player->ShowOnScreen(screenCombo->currentData().toInt());
		startMs = QDateTime::currentMSecsSinceEpoch();
		const int pc = player->PageCount();
		pageJump->setRange(1, pc > 0 ? pc : 9999);
		pageJump->setValue(1);
		RefreshUi();
	});
	connect(btnPrev, &QPushButton::clicked, this, [this]() {
		if (player) player->Prev();
	});
	connect(btnNext, &QPushButton::clicked, this, [this]() {
		if (player) player->Next();
	});
	connect(btnPrevAnim, &QPushButton::clicked, this, [this]() {
		if (player) player->PrevAnim();
	});
	connect(btnNextAnim, &QPushButton::clicked, this, [this]() {
		if (player) player->NextAnim();
	});
	connect(btnShow, &QPushButton::clicked, this, [this]() {
		if (player && player->Source()) player->ShowOnScreen(screenCombo->currentData().toInt());
	});
	connect(btnJump, &QPushButton::clicked, this, [this]() {
		if (player && player->PageCount() > 0) {
			player->SetPage(pageJump->value() - 1);
		}
	});
	connect(btnPvw, &QPushButton::toggled, this, [this](bool on) {
		if (!preview) {
			preview = new HFRPptPreviewWindow(nullptr);
		}
		if (on) {
			preview->ShowOnScreen(previewScreenCombo->currentData().toInt());
			lastPreviewPage = -2;
			lastPreviewFile.clear();
			RefreshUi();
		} else {
			preview->ClosePreview();
		}
	});
	connect(btnNotes, &QPushButton::clicked, this, [this]() {
		if (player) {
			player->SetShowNotes(!player->ShowNotes());
			btnNotes->setText(player->ShowNotes() ? QStringLiteral("隐藏备注(N)") : hfrt::T("k10025", "备注(N)", "Notes (N)"));
		}
	});
	connect(btnThumbs, &QPushButton::clicked, this, [this]() {
		if (player && player->Source()) {
			auto *dlg = new HFRPptThumbnailsDialog(player, this);
			dlg->show();
		}
	});
	connect(btnPresenter, &QPushButton::clicked, this, [this]() {
		if (player && player->Source()) {
			HfrOpenPresenterWindow(player->Source());
		}
	});
	connect(btnBlack, &QPushButton::clicked, this, [this]() {
		if (!player) return;
		player->SetBlack(!player->Black());
		btnBlack->setText(player->Black() ? QStringLiteral("取消黑屏(B)") : hfrt::T("k25152", "黑屏(B)", "Black (B)"));
	});

	UpdateQueueLabel();
	uiTimer = new QTimer(this);
	uiTimer->setInterval(300);
	connect(uiTimer, &QTimer::timeout, this, [this]() { RefreshUi(); });
	uiTimer->start();
	RefreshUi();
}

void HFRPptPlayerControl::SetPlayer(HFRPptPlayerWindow *w)
{
	player = w;
	RefreshUi();
}

void HFRPptPlayerControl::RefreshUi()
{
	if (!player) {
		return;
	}
	const int total = player->PageCount();
	if (total > 0) {
		pageLabel->setText(QStringLiteral("第 %1/%2 页 · 动画 %3/%4")
					   .arg(player->Page() + 1)
					   .arg(total)
					   .arg(player->AnimStep())
					   .arg(player->AnimStepCount()));
	} else {
		pageLabel->setText(hfrt::T("k44165", "第 -/- 页", "-/-"));
	}
	if (total > 0 && pageJump) {
		if (pageJump->maximum() != total) {
			pageJump->setRange(1, total);
		}
		if (!pageJump->hasFocus()) {
			pageJump->setValue(player->Page() + 1);
		}
	}
	/* PVW 预览：页码/文件变化时渲染"下一页"到预览窗（UI 线程 CPU 渲染，仅变化时） */
	if (preview && preview->Visible() && player->Source()) {
		const QString f = player->FilePath();
		const int page = player->Page();
		const int total = player->PageCount();
		if (total > 0 && (page != lastPreviewPage || f != lastPreviewFile)) {
			const int np = std::min(page + 1, total - 1);
			std::vector<uint8_t> buf;
			if (HfrSlidesRenderPreview(player->Source(), np, 1280, 720, buf) && !buf.empty()) {
				preview->SetImage(buf, 1280, 720);
			}
			lastPreviewPage = page;
			lastPreviewFile = f;
		}
	}
	if (timerLabel) {
		qint64 base = startMs ? startMs : 0;
		qint64 secs = base ? (QDateTime::currentMSecsSinceEpoch() - base) / 1000 : 0;
		timerLabel->setText(QStringLiteral("用时 %1:%2")
					    .arg(secs / 60, 2, 10, QLatin1Char('0'))
					    .arg(secs % 60, 2, 10, QLatin1Char('0')));
	}
}

HFRPptPlayerControl::~HFRPptPlayerControl()
{
	/* 播放窗没有父对象（独立全屏窗），随控制台一并销毁，确保内部来源在 OBS 存活时释放 */
	delete preview;
	preview = nullptr;
	delete player;
	player = nullptr;
}

void HFRPptPlayerControl::PlayQueueFile(int index)
{
	if (!player || index < 0 || index >= queueFiles.size()) {
		return;
	}
	QString err;
	if (!player->OpenFile(queueFiles.at(index), &err)) {
		fileLabel->setText(QStringLiteral("队列打开失败：") + err);
		return;
	}
	queueIndex = index;
	if (queueList) {
		queueList->setCurrentRow(index);
	}
	fileLabel->setText(QStringLiteral("文件：") + QFileInfo(queueFiles.at(index)).fileName());
	player->ShowOnScreen(screenCombo->currentData().toInt());
	startMs = QDateTime::currentMSecsSinceEpoch();
	const int pc = player->PageCount();
	pageJump->setRange(1, pc > 0 ? pc : 9999);
	pageJump->setValue(1);
	UpdateQueueLabel();
	RefreshUi();
}

void HFRPptPlayerControl::AdvanceQueue()
{
	if (queueIndex + 1 < queueFiles.size()) {
		PlayQueueFile(queueIndex + 1);
		return;
	}
	if (chkQueueLoop && chkQueueLoop->isChecked() && !queueFiles.isEmpty()) {
		PlayQueueFile(0);
		return;
	}
	if (queueIndex >= 0 && queueIndex + 1 >= queueFiles.size()) {
		fileLabel->setText(QStringLiteral("队列播放完毕：%1 个文件").arg(queueFiles.size()));
	}
}

void HFRPptPlayerControl::UpdateQueueLabel()
{
	if (!queueLabel) {
		return;
	}
	queueLabel->setText(QStringLiteral("队列 %1/%2")
				    .arg(queueIndex >= 0 ? queueIndex + 1 : 0)
				    .arg(queueFiles.size()));
}

/* ============================ 单例入口 ============================ */

static HFRPptPlayerControl *g_pptPlayerCtrl = nullptr;

void HfrOpenPptPlayerUi()
{
	if (!g_pptPlayerCtrl) {
		g_pptPlayerCtrl = new HFRPptPlayerControl(nullptr);
	}
	g_pptPlayerCtrl->show();
	g_pptPlayerCtrl->raise();
	g_pptPlayerCtrl->activateWindow();
}

void HfrShutdownPptPlayerUi()
{
	if (g_pptPlayerCtrl) {
		delete g_pptPlayerCtrl;
		g_pptPlayerCtrl = nullptr;
	}
}
