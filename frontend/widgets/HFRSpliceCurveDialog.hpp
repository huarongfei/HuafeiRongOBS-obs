#pragma once

#include <QDialog>

class QSpinBox;

/* 曲面网格控制点编辑：3×3 网格的 5 个非角点（上中/右中/下中/左中/中心）偏移。 */
class HFRSpliceCurveDialog : public QDialog {
public:
	/* 输入输出：10 个偏移（墙像素，按 cMT,cMR,cMB,cML,cC 的 x/y 顺序） */
	explicit HFRSpliceCurveDialog(const int offsets[10], QWidget *parent = nullptr);
	void GetOffsets(int out[10]) const;

private:
	QSpinBox *spins[10] = {};
};
