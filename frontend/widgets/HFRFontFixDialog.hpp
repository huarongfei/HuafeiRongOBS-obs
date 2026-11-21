#pragma once

#include <QDialog>

#include <string>
#include <vector>

#include <obs.hpp>

class QTableWidget;
class QLabel;

/* 缺字体替换向导：列出幻灯片缺失字体，为每种选择本机替代字体，一键写入来源的 font_subst。 */
class HFRFontFixDialog : public QDialog {
public:
	explicit HFRFontFixDialog(obs_source_t *source, QWidget *parent = nullptr);

private:
	void ApplyAndClose();

	obs_source_t *src = nullptr;
	QTableWidget *table = nullptr;
	std::vector<std::wstring> missing;
};
