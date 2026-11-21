#pragma once

#include <QDialog>

#include <obs.hpp>

class QTableWidget;
class QLabel;

/* 音频路由矩阵：为每个音频源勾选它进入哪些录制音轨（1..6 = mixer 位 0..5），
 * 录制时按"被使用的音轨"创建多个 AAC 编码器（ffmpeg_muxer 多轨）。 */
class HFRAudioRoutingDialog : public QDialog {
public:
	explicit HFRAudioRoutingDialog(QWidget *parent = nullptr);

private:
	void ApplyAndClose();
	void FillTable();

	QTableWidget *table = nullptr;
	QLabel *hint = nullptr;
};
