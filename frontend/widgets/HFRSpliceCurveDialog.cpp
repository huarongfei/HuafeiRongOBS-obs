#include "HFRSpliceCurveDialog.hpp"
#include "HFRStrings.hpp"

#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

static const char *kNames[5] = {"Top mid", "Right mid", "Bottom mid", "Left mid", "Center"};

HFRSpliceCurveDialog::HFRSpliceCurveDialog(const int offsets[10], QWidget *parent) : QDialog(parent)
{
	setWindowTitle(hfrt::T("k46372", "曲面网格校正（3x3 控制点）", "Curved Grid (3x3)"));
	resize(420, 320);

	QVBoxLayout *root = new QVBoxLayout(this);
	QLabel *hint = new QLabel(this);
	hint->setWordWrap(true);
	hint->setText(hfrt::T("hint.curve",
			      "给 3x3 网格的 5 个非角点设置偏移（墙像素，正右/正下），配合四角校正可做曲面/弧形屏。",
			      "Offset the 5 inner control points of the 3x3 grid (wall px, +x right / +y down). Combined with corner correction this matches curved/arc screens."));
	root->addWidget(hint);

	QGridLayout *grid = new QGridLayout();
	grid->addWidget(new QLabel(QStringLiteral("控制点"), this), 0, 0);
	grid->addWidget(new QLabel(QStringLiteral("X"), this), 0, 1);
	grid->addWidget(new QLabel(QStringLiteral("Y"), this), 0, 2);
	for (int p = 0; p < 5; p++) {
		grid->addWidget(new QLabel(QString::fromUtf8(kNames[p]), this), p + 1, 0);
		for (int k = 0; k < 2; k++) {
			QSpinBox *sp = new QSpinBox(this);
			sp->setRange(-4000, 4000);
			sp->setValue(offsets[p * 2 + k]);
			grid->addWidget(sp, p + 1, k + 1);
			spins[p * 2 + k] = sp;
		}
	}
	root->addLayout(grid);
	root->addStretch(1);

	QDialogButtonBox *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	box->button(QDialogButtonBox::Ok)->setText(hfrt::T("k88019", "确定", "OK"));
	box->button(QDialogButtonBox::Cancel)->setText(hfrt::T("k91265", "取消", "Cancel"));
	root->addWidget(box);
	connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void HFRSpliceCurveDialog::GetOffsets(int out[10]) const
{
	for (int i = 0; i < 10; i++) {
		out[i] = spins[i] ? spins[i]->value() : 0;
	}
}
