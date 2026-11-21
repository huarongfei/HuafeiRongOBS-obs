#include "HFRSpliceDialog.hpp"
#include "HFRStrings.hpp"

#include "HFRSpliceCurveDialog.hpp"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDialogButtonBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

HFRSpliceDialog::HFRSpliceDialog(obs_canvas_t *canvas_, const QVector<HfrWallTile> &initial, ApplyFn onApply,
				 QWidget *parent)
	: QDialog(parent), canvas(canvas_), applyFn(std::move(onApply))
{
	setWindowTitle(hfrt::T("k5825", "异形屏拼接（分块投影）", "Splice Projection (Tiles)"));
	resize(1460, 520);

	struct obs_video_info ovi = {};
	if (canvas && obs_canvas_get_video_info(canvas, &ovi) && ovi.base_width && ovi.base_height) {
		canvasW = (int)ovi.base_width;
		canvasH = (int)ovi.base_height;
	}

	QVBoxLayout *root = new QVBoxLayout(this);

	const QString canvasName = canvas ? QString::fromUtf8(obs_canvas_get_name(canvas)) : QString();
	infoLabel = new QLabel(this);
	infoLabel->setWordWrap(true);
	infoLabel->setText(QStringLiteral("画布「%1」= 拼接墙：%2 × %3 像素。"
					  "每行 = 一块物理屏，填它显示墙上的哪块矩形（像素坐标）。"
					  "应用后：该画布现有投影会被替换为这些分块投影。")
				   .arg(canvasName.isEmpty() ? QStringLiteral("?") : canvasName)
				   .arg(canvasW)
				   .arg(canvasH) +
				   QStringLiteral("\n边缘融合：相邻块在墙上重叠时，给重叠边填「融合宽」像素，"
						  "本块边缘渐暗、相邻块互补渐亮，叠加后亮度连续（旋转块暂不融合）。"
						  "\n四角校正：左上/右上/左下/右下 X,Y = 该角在墙像素坐标下的位移（正右/正下），"
						  "用于梯形/曲面屏几何校正（与旋转暂不叠加）。"
						  "\n色彩统一：伽马(0.2–3.0)/亮度(-0.3–0.3)/增益(0.2–3.0) 按块调整投影机色差。"));
	root->addWidget(infoLabel);

	table = new QTableWidget(0, 22, this);
	table->setHorizontalHeaderLabels({hfrt::T("k10059", "屏幕（编号-分辨率）", "Screen (index-res)"), QStringLiteral("X"),
					  QStringLiteral("Y"), hfrt::T("k25150", "宽", "W"), hfrt::T("k18071", "高", "H"),
					  hfrt::T("k1639", "旋转", "Rot"), QStringLiteral("融合左"), QStringLiteral("融合右"),
					  QStringLiteral("融合上"), QStringLiteral("融合下"),
					  QStringLiteral("左上X"), QStringLiteral("左上Y"), QStringLiteral("右上X"),
					  QStringLiteral("右上Y"), QStringLiteral("左下X"), QStringLiteral("左下Y"),
					  QStringLiteral("右下X"), QStringLiteral("右下Y"),
					  hfrt::T("k22361", "伽马", "Gamma"), hfrt::T("k75378", "亮度", "Lift"), hfrt::T("k43869", "增益", "Gain"),
					  hfrt::T("k970", "曲面", "Curve")});
	table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
	table->verticalHeader()->setVisible(false);
	table->setSelectionBehavior(QAbstractItemView::SelectRows);
	root->addWidget(table, 1);

	QHBoxLayout *btns = new QHBoxLayout();
	QPushButton *btnAdd = new QPushButton(QStringLiteral("+ 添加分块"), this);
	QPushButton *btnDel = new QPushButton(QStringLiteral("删除选中"), this);
	QPushButton *btnDefault = new QPushButton(QStringLiteral("按屏幕等分重排"), this);
	btns->addWidget(btnAdd);
	btns->addWidget(btnDel);
	btns->addWidget(btnDefault);
	btns->addStretch(1);
	root->addLayout(btns);

	QDialogButtonBox *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	box->button(QDialogButtonBox::Ok)->setText(hfrt::T("k65333", "应用分块投影", "Apply tiles"));
	box->button(QDialogButtonBox::Cancel)->setText(hfrt::T("k73717", "取消", "Cancel"));
	root->addWidget(box);

	connect(btnAdd, &QPushButton::clicked, this, [this]() {
		HfrWallTile t;
		const auto screens = QGuiApplication::screens();
		t.screen = table->rowCount() < screens.size() ? table->rowCount() : -1;
		t.x = 0;
		t.y = 0;
		t.w = canvasW > 0 ? canvasW : 1920;
		t.h = canvasH > 0 ? canvasH : 1080;
		AddRow(t);
	});
	connect(btnDel, &QPushButton::clicked, this, [this]() { RemoveSelectedRow(); });
	connect(btnDefault, &QPushButton::clicked, this, [this]() { RebuildDefaultTiles(); });
	connect(box, &QDialogButtonBox::accepted, this, [this]() {
		const std::vector<HfrWallTile> tiles = CollectTiles();
		if (tiles.empty()) {
			return;
		}
		if (applyFn) {
			QVector<HfrWallTile> qv;
			for (const HfrWallTile &t : tiles) {
				qv.push_back(t);
			}
			applyFn(canvas, qv);
		}
		accept();
	});
	connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);

	if (initial.isEmpty()) {
		RebuildDefaultTiles();
	} else {
		for (const HfrWallTile &t : initial) {
			AddRow(t);
		}
	}
}

void HFRSpliceDialog::AddRow(const HfrWallTile &t)
{
	const auto screens = QGuiApplication::screens();
	const int row = table->rowCount();
	table->insertRow(row);

	QComboBox *screenCombo = new QComboBox(table);
	for (int i = 0; i < screens.size(); i++) {
		const QScreen *s = screens.at(i);
		screenCombo->addItem(QStringLiteral("屏%1 · %2x%3")
					     .arg(i)
					     .arg(s->geometry().width())
					     .arg(s->geometry().height()),
				     i);
	}
	if (t.screen >= 0 && t.screen < screens.size()) {
		screenCombo->setCurrentIndex(t.screen);
	}
	table->setCellWidget(row, 0, screenCombo);

	auto makeSpin = [this](int value, int maxV) {
		QSpinBox *sp = new QSpinBox(table);
		sp->setRange(0, maxV > 0 ? maxV : 100000);
		sp->setValue(value);
		return sp;
	};
	table->setCellWidget(row, 1, makeSpin(t.x, canvasW));
	table->setCellWidget(row, 2, makeSpin(t.y, canvasH));
	table->setCellWidget(row, 3, makeSpin(t.w, canvasW));
	table->setCellWidget(row, 4, makeSpin(t.h, canvasH));

	auto makeSignedSpin = [this](int value) {
		QSpinBox *sp = new QSpinBox(table);
		sp->setRange(-4000, 4000);
		sp->setValue(value);
		return sp;
	};
	table->setCellWidget(row, 10, makeSignedSpin(t.kTLx));
	table->setCellWidget(row, 11, makeSignedSpin(t.kTLy));
	table->setCellWidget(row, 12, makeSignedSpin(t.kTRx));
	table->setCellWidget(row, 13, makeSignedSpin(t.kTRy));
	table->setCellWidget(row, 14, makeSignedSpin(t.kBLx));
	table->setCellWidget(row, 15, makeSignedSpin(t.kBLy));
	table->setCellWidget(row, 16, makeSignedSpin(t.kBRx));
	table->setCellWidget(row, 17, makeSignedSpin(t.kBRy));

	auto makeDbl = [this](double value, double minV, double maxV, double step) {
		QDoubleSpinBox *sp = new QDoubleSpinBox(table);
		sp->setRange(minV, maxV);
		sp->setSingleStep(step);
		sp->setDecimals(2);
		sp->setValue(value);
		return sp;
	};
	table->setCellWidget(row, 18, makeDbl(t.gammaV, 0.20, 3.00, 0.05));
	table->setCellWidget(row, 19, makeDbl(t.liftV, -0.30, 0.30, 0.01));
	table->setCellWidget(row, 20, makeDbl(t.gainV, 0.20, 3.00, 0.05));

	rowData.append(t);
	QPushButton *curveBtn = new QPushButton(QStringLiteral("曲面…"), table);
	curveBtn->setToolTip(QStringLiteral("3x3 控制点曲面/弧形屏校正（与四角校正叠加）"));
	table->setCellWidget(row, 21, curveBtn);
	connect(curveBtn, &QPushButton::clicked, this, [this, curveBtn]() {
		for (int r = 0; r < table->rowCount(); r++) {
			if (table->cellWidget(r, 21) == curveBtn && r < rowData.size()) {
				int offs[10] = {rowData[r].cMTx, rowData[r].cMTy, rowData[r].cMRx, rowData[r].cMRy,
						rowData[r].cMBx, rowData[r].cMBy, rowData[r].cMLx, rowData[r].cMLy,
						rowData[r].cCx,  rowData[r].cCy};
				HFRSpliceCurveDialog dlg(offs, this);
				if (dlg.exec() == QDialog::Accepted) {
					HfrWallTile &tile = rowData[r];
					dlg.GetOffsets(offs);
					tile.cMTx = offs[0];
					tile.cMTy = offs[1];
					tile.cMRx = offs[2];
					tile.cMRy = offs[3];
					tile.cMBx = offs[4];
					tile.cMBy = offs[5];
					tile.cMLx = offs[6];
					tile.cMLy = offs[7];
					tile.cCx = offs[8];
					tile.cCy = offs[9];
				}
				break;
			}
		}
	});

	table->setCellWidget(row, 6, makeSpin(t.blendL, canvasW));
	table->setCellWidget(row, 7, makeSpin(t.blendR, canvasW));
	table->setCellWidget(row, 8, makeSpin(t.blendT, canvasH));
	table->setCellWidget(row, 9, makeSpin(t.blendB, canvasH));

	table->setColumnWidth(0, 180);
	table->setColumnWidth(5, 70);

	QComboBox *rotCombo = new QComboBox(table);
	rotCombo->addItem(QStringLiteral("0°"), 0);
	rotCombo->addItem(QStringLiteral("90°"), 90);
	rotCombo->addItem(QStringLiteral("180°"), 180);
	rotCombo->addItem(QStringLiteral("270°"), 270);
	const int rotIdx = rotCombo->findData(t.rot);
	rotCombo->setCurrentIndex(rotIdx >= 0 ? rotIdx : 0);
	table->setCellWidget(row, 5, rotCombo);
}

void HFRSpliceDialog::RemoveSelectedRow()
{
	const int row = table->currentRow();
	if (row >= 0) {
		table->removeRow(row);
		if (row < rowData.size()) {
			rowData.removeAt(row);
		}
	}
}

void HFRSpliceDialog::RebuildDefaultTiles()
{
	table->setRowCount(0);
	rowData.clear();
	const auto screens = QGuiApplication::screens();
	const int n = screens.isEmpty() ? 1 : screens.size();
	const int tileW = (canvasW > 0 && n > 0) ? canvasW / n : (canvasW > 0 ? canvasW : 1920);
	for (int i = 0; i < n; i++) {
		HfrWallTile t;
		t.screen = i;
		t.x = i * tileW;
		t.y = 0;
		t.w = (i == n - 1) ? (canvasW - t.x) : tileW;
		t.h = canvasH;
		AddRow(t);
	}
}

std::vector<HfrWallTile> HFRSpliceDialog::CollectTiles() const
{
	std::vector<HfrWallTile> out;
	for (int r = 0; r < table->rowCount(); r++) {
		HfrWallTile t;
		QComboBox *sc = qobject_cast<QComboBox *>(table->cellWidget(r, 0));
		QSpinBox *sx = qobject_cast<QSpinBox *>(table->cellWidget(r, 1));
		QSpinBox *sy = qobject_cast<QSpinBox *>(table->cellWidget(r, 2));
		QSpinBox *sw = qobject_cast<QSpinBox *>(table->cellWidget(r, 3));
		QSpinBox *sh = qobject_cast<QSpinBox *>(table->cellWidget(r, 4));
		QComboBox *rc = qobject_cast<QComboBox *>(table->cellWidget(r, 5));
		QSpinBox *bl = qobject_cast<QSpinBox *>(table->cellWidget(r, 6));
		QSpinBox *br = qobject_cast<QSpinBox *>(table->cellWidget(r, 7));
		QSpinBox *bt = qobject_cast<QSpinBox *>(table->cellWidget(r, 8));
		QSpinBox *bb = qobject_cast<QSpinBox *>(table->cellWidget(r, 9));
		QSpinBox *ks[8];
		bool kok = true;
		for (int c = 0; c < 8; c++) {
			ks[c] = qobject_cast<QSpinBox *>(table->cellWidget(r, 10 + c));
			if (!ks[c]) {
				kok = false;
			}
		}
		QDoubleSpinBox *gm = qobject_cast<QDoubleSpinBox *>(table->cellWidget(r, 18));
		QDoubleSpinBox *lf = qobject_cast<QDoubleSpinBox *>(table->cellWidget(r, 19));
		QDoubleSpinBox *gn = qobject_cast<QDoubleSpinBox *>(table->cellWidget(r, 20));
		if (!sc || !sx || !sy || !sw || !sh || !rc || !bl || !br || !bt || !bb || !kok || !gm || !lf ||
		    !gn) {
			continue;
		}
		t.screen = sc->currentData().toInt();
		t.x = sx->value();
		t.y = sy->value();
		t.w = sw->value();
		t.h = sh->value();
		t.rot = rc->currentData().toInt();
		t.blendL = bl->value();
		t.blendR = br->value();
		t.blendT = bt->value();
		t.blendB = bb->value();
		t.kTLx = ks[0]->value();
		t.kTLy = ks[1]->value();
		t.kTRx = ks[2]->value();
		t.kTRy = ks[3]->value();
		t.kBLx = ks[4]->value();
		t.kBLy = ks[5]->value();
		t.kBRx = ks[6]->value();
		t.kBRy = ks[7]->value();
		t.gammaV = gm->value();
		t.liftV = lf->value();
		t.gainV = gn->value();
		/* 隐藏字段（曲面网格偏移）来自行数据 */
		if (r < rowData.size()) {
			t.cMTx = rowData[r].cMTx;
			t.cMTy = rowData[r].cMTy;
			t.cMRx = rowData[r].cMRx;
			t.cMRy = rowData[r].cMRy;
			t.cMBx = rowData[r].cMBx;
			t.cMBy = rowData[r].cMBy;
			t.cMLx = rowData[r].cMLx;
			t.cMLy = rowData[r].cMLy;
			t.cCx = rowData[r].cCx;
			t.cCy = rowData[r].cCy;
		}
		if (t.w <= 0 || t.h <= 0) {
			continue;
		}
		out.push_back(t);
	}
	return out;
}
