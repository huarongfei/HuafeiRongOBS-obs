#include "HFRFontFixDialog.hpp"
#include "HFRStrings.hpp"

#include "HFRSlidesSource.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

/* 常用字体 → 建议替代（按优先级） */
static QStringList SuggestFor(const QString &from)
{
	const QString f = from.toLower();
	if (f.contains(QStringLiteral("宋")) || f.contains(QStringLiteral("simsun")) ||
	    f.contains(QStringLiteral("song"))) {
		return {QStringLiteral("微软雅黑"), QStringLiteral("Microsoft YaHei"), QStringLiteral("等线"),
			QStringLiteral("宋体")};
	}
	if (f.contains(QStringLiteral("黑体")) || f.contains(QStringLiteral("simhei"))) {
		return {QStringLiteral("微软雅黑"), QStringLiteral("Microsoft YaHei"), QStringLiteral("黑体")};
	}
	if (f.contains(QStringLiteral("楷"))) {
		return {QStringLiteral("楷体"), QStringLiteral("KaiTi"), QStringLiteral("微软雅黑")};
	}
	if (f.contains(QStringLiteral("calibri"))) {
		return {QStringLiteral("Segoe UI"), QStringLiteral("微软雅黑")};
	}
	if (f.contains(QStringLiteral("arial"))) {
		return {QStringLiteral("Arial"), QStringLiteral("Segoe UI")};
	}
	if (f.contains(QStringLiteral("yahei")) || f.contains(QStringLiteral("雅黑"))) {
		return {QStringLiteral("等线"), QStringLiteral("DengXian"), QStringLiteral("黑体")};
	}
	return {};
}

HFRFontFixDialog::HFRFontFixDialog(obs_source_t *source, QWidget *parent) : QDialog(parent), src(source)
{
	setWindowTitle(hfrt::T("k68523", "字体修复向导", "Font Fix Wizard"));
	resize(660, 440);

	QVBoxLayout *root = new QVBoxLayout(this);
	const QString srcName = src ? QString::fromUtf8(obs_source_get_name(src)) : QString();
	missing = src ? HfrSlidesMissingFonts(src) : std::vector<std::wstring>();

	QLabel *info = new QLabel(this);
	info->setWordWrap(true);
	if (missing.empty()) {
		info->setText(hfrt::T("font.ok", "来源「%1」字体齐全，无需替换。",
				      "Source %1: all fonts available, nothing to replace.")
				      .arg(srcName));
	} else {
		info->setText(hfrt::T("font.miss",
				      "来源「%1」缺失 %2 种字体。为每种选择本机替代字体（保持原样 = 不替换），确定后写入来源设置并立即重建页面。",
				      "Source %1 is missing %2 font(s). Pick an installed substitute for each (keep = no replacement). Settings are written and the page rebuilt immediately.")
				      .arg(srcName)
				      .arg(missing.size()));
	}
	root->addWidget(info);

	table = new QTableWidget((int)missing.size(), 2, this);
	table->setHorizontalHeaderLabels({hfrt::T("k1356", "缺失字体", "Missing font"), hfrt::T("k24074", "替换为", "Replace with")});
	table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
	table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
	table->verticalHeader()->setVisible(false);
	root->addWidget(table, 1);

	const QStringList families = QFontDatabase::families();
	for (int row = 0; row < (int)missing.size(); row++) {
		const QString from = QString::fromWCharArray(missing[(size_t)row].c_str());
		table->setItem(row, 0, new QTableWidgetItem(from));

		QComboBox *combo = new QComboBox(table);
		combo->addItem(QStringLiteral("（保持原样/不替换）"), QString());
		for (const QString &fam : families) {
			combo->addItem(fam, fam);
		}
		/* 按建议选默认 */
		int pick = 0;
		const QStringList suggest = SuggestFor(from);
		for (const QString &s : suggest) {
			const int idx = combo->findData(s);
			if (idx >= 0) {
				pick = idx;
				break;
			}
		}
		combo->setCurrentIndex(pick);
		table->setCellWidget(row, 1, combo);
	}

	QDialogButtonBox *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	box->button(QDialogButtonBox::Ok)->setText(hfrt::T("k24871", "应用替换", "Apply"));
	box->button(QDialogButtonBox::Cancel)->setText(hfrt::T("k95535", "取消", "Cancel"));
	root->addWidget(box);
	connect(box, &QDialogButtonBox::accepted, this, [this]() { ApplyAndClose(); });
	connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void HFRFontFixDialog::ApplyAndClose()
{
	QStringList pairs;
	for (int row = 0; row < table->rowCount(); row++) {
		QTableWidgetItem *item = table->item(row, 0);
		QComboBox *combo = qobject_cast<QComboBox *>(table->cellWidget(row, 1));
		if (!item || !combo) {
			continue;
		}
		const QString to = combo->currentData().toString();
		if (to.isEmpty()) {
			continue;
		}
		pairs << (item->text() + QStringLiteral("=") + to);
	}
	if (!pairs.isEmpty() && src) {
		HfrSlidesSetFontSubst(src, pairs.join(QStringLiteral(";")));
	}
	accept();
}
