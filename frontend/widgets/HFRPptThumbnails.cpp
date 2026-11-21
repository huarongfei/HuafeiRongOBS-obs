#include "HFRPptThumbnails.hpp"
#include "HFRStrings.hpp"

#include "HFRPptPlayer.hpp"
#include "HFRSlidesSource.hpp"

#include <QImage>
#include <QLabel>
#include <QListWidget>
#include <QPixmap>
#include <QTimer>
#include <QVBoxLayout>

HFRPptThumbnailsDialog::HFRPptThumbnailsDialog(HFRPptPlayerWindow *player_, QWidget *parent)
	: QDialog(parent), player(player_)
{
	setWindowTitle(hfrt::T("k78435", "缩略图总览", "Thumbnails"));
	setAttribute(Qt::WA_DeleteOnClose, true);
	resize(820, 560);

	QVBoxLayout *root = new QVBoxLayout(this);
	root->addWidget(new QLabel(hfrt::T("hint.thumb",
					 "单击缩略图跳页；当前页自动高亮。缩略图后台分批渲染，不阻塞播放。",
					 "Click a thumbnail to jump; the current page is highlighted. Thumbnails render in batches in the background."),
				   this));

	list = new QListWidget(this);
	list->setViewMode(QListView::IconMode);
	list->setResizeMode(QListView::Adjust);
	list->setGridSize(QSize(180, 124));
	list->setIconSize(QSize(160, 90));
	list->setSpacing(6);
	list->setMovement(QListView::Static);
	root->addWidget(list, 1);

	const int total = player ? player->PageCount() : 0;
	for (int i = 0; i < total; i++) {
		QListWidgetItem *item = new QListWidgetItem(QStringLiteral("第 %1 页").arg(i + 1), list);
		item->setSizeHint(QSize(170, 116));
	}
	if (total == 0) {
		list->addItem(new QListWidgetItem(QStringLiteral("（未载入演示文稿）"), list));
	}

	connect(list, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
		if (!player || !item) {
			return;
		}
		const int row = list->row(item);
		if (row >= 0 && row < player->PageCount()) {
			player->SetPage(row);
		}
	});

	batchTimer = new QTimer(this);
	batchTimer->setInterval(20);
	connect(batchTimer, &QTimer::timeout, this, [this]() { RenderNextBatch(); });
	if (total > 0) {
		batchTimer->start();
	}

	selTimer = new QTimer(this);
	selTimer->setInterval(400);
	connect(selTimer, &QTimer::timeout, this, [this]() { SyncSelection(); });
	selTimer->start();
	SyncSelection();
}

void HFRPptThumbnailsDialog::RenderNextBatch()
{
	if (!player || !player->Source()) {
		if (batchTimer) {
			batchTimer->stop();
		}
		return;
	}
	const int total = player->PageCount();
	const int kBatch = 4;
	for (int n = 0; n < kBatch && nextIndex < total; n++, nextIndex++) {
		QListWidgetItem *item = list->item(nextIndex);
		if (!item) {
			continue;
		}
		std::vector<uint8_t> buf;
		if (HfrSlidesRenderPreview(player->Source(), nextIndex, 160, 90, buf) && !buf.empty()) {
			const QImage img((const uchar *)buf.data(), 160, 90, 160 * 4, QImage::Format_ARGB32);
			item->setIcon(QPixmap::fromImage(img.copy()));
			item->setText(QStringLiteral("第 %1 页").arg(nextIndex + 1));
		} else {
			item->setText(QStringLiteral("第 %1 页（渲染失败）").arg(nextIndex + 1));
		}
	}
	if (nextIndex >= total) {
		batchTimer->stop();
	}
}

void HFRPptThumbnailsDialog::SyncSelection()
{
	if (!player || !player->Source()) {
		return;
	}
	const int cur = player->Page();
	if (cur >= 0 && cur < list->count() && list->currentRow() != cur) {
		list->setCurrentRow(cur);
	}
}
