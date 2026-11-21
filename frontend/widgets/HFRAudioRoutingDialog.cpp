#include "HFRAudioRoutingDialog.hpp"
#include "HFRStrings.hpp"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <string>
#include <vector>

HFRAudioRoutingDialog::HFRAudioRoutingDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(hfrt::T("k25519", "音频路由（录制音轨分配）", "Audio Routing (recording tracks)"));
	resize(720, 460);

	QVBoxLayout *root = new QVBoxLayout(this);
	hint = new QLabel(this);
	hint->setWordWrap(true);
	hint->setText(hfrt::T("hint.audio",
			      "勾选=该音源进入对应录制音轨（1=现场混音默认轨，2..6=附加轨，如解说/音乐）。每画布录制会包含所有「被使用」的音轨（0..最大轨连续创建）。注意：音源只勾选第 2 轨时不会出现在现场/第 1 轨。",
			      "Check = this source is included in that recording track (1 = program mix, 2..6 = extra tracks such as commentator/music). Each canvas recording creates all used tracks (0..max, contiguous). Note: a source checked only on track 2 will not be heard in the program mix / track 1."));
	root->addWidget(hint);

	table = new QTableWidget(0, 7, this);
	table->setHorizontalHeaderLabels({hfrt::T("k883", "音频源", "Audio source"), QStringLiteral("轨1"), QStringLiteral("轨2"),
					  QStringLiteral("轨3"), QStringLiteral("轨4"), QStringLiteral("轨5"),
					  QStringLiteral("轨6")});
	table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
	table->verticalHeader()->setVisible(false);
	root->addWidget(table, 1);

	QDialogButtonBox *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	box->button(QDialogButtonBox::Ok)->setText(hfrt::T("k90494", "应用路由", "Apply routing"));
	box->button(QDialogButtonBox::Cancel)->setText(hfrt::T("k8781", "取消", "Cancel"));
	root->addWidget(box);
	connect(box, &QDialogButtonBox::accepted, this, [this]() { ApplyAndClose(); });
	connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);

	FillTable();
}

void HFRAudioRoutingDialog::FillTable()
{
	std::vector<obs_source_t *> sources;
	auto enumCb = [](void *param, obs_source_t *src) -> bool {
		auto *vec = static_cast<std::vector<obs_source_t *> *>(param);
		if (src && (obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO)) {
			vec->push_back(src);
		}
		return true;
	};
	obs_enum_sources(enumCb, &sources);

	table->setRowCount((int)sources.size());
	for (int row = 0; row < (int)sources.size(); row++) {
		obs_source_t *src = sources[(size_t)row];
		const QString name = QString::fromUtf8(obs_source_get_name(src));
		table->setItem(row, 0, new QTableWidgetItem(name));
		const uint32_t mask = obs_source_get_audio_mixers(src);
		for (int t = 0; t < 6; t++) {
			QCheckBox *cb = new QCheckBox(table);
			cb->setChecked((mask & (1u << t)) != 0);
			table->setCellWidget(row, 1 + t, cb);
		}
	}
}

void HFRAudioRoutingDialog::ApplyAndClose()
{
	int changed = 0;
	for (int row = 0; row < table->rowCount(); row++) {
		QTableWidgetItem *item = table->item(row, 0);
		if (!item) {
			continue;
		}
		obs_source_t *src = obs_get_source_by_name(item->text().toUtf8().constData());
		if (!src) {
			continue;
		}
		uint32_t mask = 0;
		for (int t = 0; t < 6; t++) {
			QCheckBox *cb = qobject_cast<QCheckBox *>(table->cellWidget(row, 1 + t));
			if (cb && cb->isChecked()) {
				mask |= (1u << t);
			}
		}
		if (mask == 0) {
			mask = 1u; /* 不允许全空：至少现场轨，避免源完全静音 */
		}
		obs_source_set_audio_mixers(src, mask);
		obs_source_release(src);
		changed++;
	}
	accept();
}
