#pragma once

#include <QDialog>

/* 快捷键/远程热键帮助：集中展示 HFR 前端热键名、画布级动态热键与界面快捷键。 */
class HFRHotkeyHelpDialog : public QDialog {
public:
	explicit HFRHotkeyHelpDialog(QWidget *parent = nullptr);
};
