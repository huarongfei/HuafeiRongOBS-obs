#include "HFRPresenter.hpp"

#include "HFRPpt.hpp"
#include "HFRPptSource.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QTextEdit>
#include <QVBoxLayout>

#include <obs-module.h>

static HfrPresenterWindow *g_presenter = nullptr;

HfrPresenterWindow::HfrPresenterWindow(QWidget *parent) : QWidget(parent)
{
	setWindowTitle(QStringLiteral("讲者视图（备注 / 下一页）"));
	setAttribute(Qt::WA_DeleteOnClose, false);
	resize(1100, 620);

	QHBoxLayout *root = new QHBoxLayout(this);

	curView = new QLabel(this);
	curView->setMinimumSize(560, 315);
	curView->setStyleSheet(QStringLiteral("background:#111;color:#888;"));
	curView->setAlignment(Qt::AlignCenter);
	curView->setText(QStringLiteral("当前页"));
	root->addWidget(curView, 3);

	QVBoxLayout *right = new QVBoxLayout();
	nextView = new QLabel(this);
	nextView->setMinimumSize(300, 169);
	nextView->setStyleSheet(QStringLiteral("background:#111;color:#888;"));
	nextView->setAlignment(Qt::AlignCenter);
	nextView->setText(QStringLiteral("下一页"));
	right->addWidget(nextView, 1);

	pageLabel = new QLabel(QStringLiteral("第 -/- 页"), this);
	right->addWidget(pageLabel);

	right->addWidget(new QLabel(QStringLiteral("备注："), this));
	notesView = new QTextEdit(this);
	notesView->setReadOnly(true);
	notesView->setPlaceholderText(QStringLiteral("（该版本未从文档取到备注文本；后续将接入 UNO 备注读取）"));
	right->addWidget(notesView, 3);

	QHBoxLayout *btns = new QHBoxLayout();
	btnPrev = new QPushButton(QStringLiteral("◀ 上一页"), this);
	btnNext = new QPushButton(QStringLiteral("下一页 ▶"), this);
	btns->addWidget(btnPrev);
	btns->addWidget(btnNext);
	right->addLayout(btns);

	globalKeys = new QCheckBox(QStringLiteral("全局翻页（翻页笔/键盘，焦点不在本窗口也生效）"), this);
	right->addWidget(globalKeys);

	root->addLayout(right, 2);

	connect(btnPrev, &QPushButton::clicked, this, [this]() { GotoPrev(); });
	connect(btnNext, &QPushButton::clicked, this, [this]() { GotoNext(); });
	connect(globalKeys, &QCheckBox::toggled, this, [this](bool on) {
		if (on) {
			qApp->installEventFilter(this);
		} else {
			qApp->removeEventFilter(this);
		}
	});

	setFocusPolicy(Qt::StrongFocus);
	UpdateImages();
	UpdateNotes();
}

HfrPresenterWindow::~HfrPresenterWindow()
{
	if (globalKeys && globalKeys->isChecked()) {
		qApp->removeEventFilter(this);
	}
	if (src) {
		obs_source_release(src);
		src = nullptr;
	}
	g_presenter = nullptr;
}

void HfrPresenterWindow::BindSource(obs_source_t *source)
{
	if (src) {
		obs_source_release(src);
		src = nullptr;
	}
	if (source) {
		src = obs_source_get_ref(source);
	}
	RefreshAll();
}

HFRPptDocument *HfrPresenterWindow::Doc() const
{
	if (src) {
		if (HFRPptDocument *d = HfrPptSourceGetDoc(src)) {
			return d;
		}
	}
	/* 未绑定来源时不提供文档（请先在场景/画布里添加 PPT 来源） */
	return nullptr;
}

void HfrPresenterWindow::GotoNext()
{
	HFRPptDocument *d = Doc();
	if (!d || !d->IsOpen()) {
		return;
	}
	d->NextPart();
	if (src) {
		HfrPptSourceRenderNow(src); /* UI 线程内完成 LOK 渲染 */
		HfrPptSourceMarkDirty(src);
	}
	RefreshAll();
}

void HfrPresenterWindow::GotoPrev()
{
	HFRPptDocument *d = Doc();
	if (!d || !d->IsOpen()) {
		return;
	}
	d->PrevPart();
	if (src) {
		HfrPptSourceRenderNow(src);
		HfrPptSourceMarkDirty(src);
	}
	RefreshAll();
}

static QPixmap PixmapFromBgra(const std::vector<uint8_t> &buf, uint32_t w, uint32_t h)
{
	if (buf.empty() || w == 0 || h == 0) {
		return QPixmap();
	}
	QImage img((const uchar *)buf.data(), (int)w, (int)h, (int)(w * 4), QImage::Format_ARGB32);
	return QPixmap::fromImage(img.copy());
}

void HfrPresenterWindow::UpdateImages()
{
	HFRPptDocument *d = Doc();
	if (!d || !d->IsOpen()) {
		curView->setText(QStringLiteral("未绑定 PPT：请先在画布/场景里添加\"PPT 演示文稿\"来源"));
		nextView->clear();
		pageLabel->setText(QStringLiteral("第 -/- 页"));
		return;
	}
	const int cur = d->CurrentPart();
	const int total = d->PartCount();

	std::vector<uint8_t> buf;
	QString err;
	if (d->RenderPart(cur, 960, 540, buf, &err)) {
		curView->setPixmap(PixmapFromBgra(buf, 960, 540).scaled(curView->size(), Qt::KeepAspectRatio,
									  Qt::SmoothTransformation));
	}
	if (cur + 1 < total && d->RenderPart(cur + 1, 480, 270, buf, &err)) {
		nextView->setPixmap(PixmapFromBgra(buf, 480, 270).scaled(nextView->size(), Qt::KeepAspectRatio,
									  Qt::SmoothTransformation));
	} else {
		nextView->setText(QStringLiteral("（已是最后一页）"));
	}
	pageLabel->setText(QStringLiteral("第 %1 / %2 页").arg(cur + 1).arg(total));
}

void HfrPresenterWindow::UpdateNotes()
{
	HFRPptDocument *d = Doc();
	const QString notes = (d && d->IsOpen()) ? d->NotesText() : QString();
	notesView->setPlainText(notes);
}

void HfrPresenterWindow::RefreshAll()
{
	UpdateImages();
	UpdateNotes();
}

bool HfrPresenterWindow::eventFilter(QObject *watched, QEvent *event)
{
	if (event->type() == QEvent::KeyPress) {
		QKeyEvent *ke = static_cast<QKeyEvent *>(event);
		switch (ke->key()) {
		case Qt::Key_PageDown:
		case Qt::Key_Right:
		case Qt::Key_Space:
		case Qt::Key_Down:
			GotoNext();
			return true;
		case Qt::Key_PageUp:
		case Qt::Key_Left:
		case Qt::Key_Up:
			GotoPrev();
			return true;
		default:
			break;
		}
	}
	return QWidget::eventFilter(watched, event);
}

void HfrPresenterWindow::closeEvent(QCloseEvent *event)
{
	Q_UNUSED(event);
	/* 窗口常驻：关闭仅隐藏，避免反复重建 */
	hide();
}

void HfrOpenPresenterWindow()
{
	if (!g_presenter) {
		g_presenter = new HfrPresenterWindow();
	}
	obs_source_t *src = HfrFindFirstPptSource();
	if (src) {
		g_presenter->BindSource(src);
	}
	g_presenter->show();
	g_presenter->raise();
	g_presenter->activateWindow();
}