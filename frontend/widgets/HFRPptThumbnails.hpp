#pragma once

#include <QDialog>

class QListWidget;
class QTimer;
class HFRPptPlayerWindow;

/* 播放器缩略图总览：网格显示各页缩略图（后台分批渲染），单击跳页，当前页高亮跟随。 */
class HFRPptThumbnailsDialog : public QDialog {
public:
	explicit HFRPptThumbnailsDialog(HFRPptPlayerWindow *player, QWidget *parent = nullptr);

private:
	void RenderNextBatch();
	void SyncSelection();

	HFRPptPlayerWindow *player = nullptr;
	QListWidget *list = nullptr;
	QTimer *batchTimer = nullptr;
	QTimer *selTimer = nullptr;
	int nextIndex = 0;
};
