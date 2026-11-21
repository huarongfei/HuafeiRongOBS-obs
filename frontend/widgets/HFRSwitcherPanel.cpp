#include "HFRSwitcherPanel.hpp"

#include "HFRStrings.hpp"

#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <obs-module.h>

namespace {
bool CollectScenesCb(void *param, obs_source_t *sceneSource)
{
	auto *vec = static_cast<std::vector<obs_source_t *> *>(param);
	if (vec && sceneSource) {
		vec->push_back(sceneSource);
	}
	return true;
}
} // namespace

HFRSwitcherPanel::HFRSwitcherPanel(QWidget *parent) : QWidget(parent)
{
	QVBoxLayout *root = new QVBoxLayout(this);
	root->setContentsMargins(4, 6, 4, 4);
	root->setSpacing(6);

	scopeLabel = new QLabel(hfrt::T("sw.scope", "作用域：未选中画布", "Scope: no canvas selected"), this);
	scopeLabel->setStyleSheet(QStringLiteral("font-weight:bold;"));
	root->addWidget(scopeLabel);

	QLabel *hint = new QLabel(hfrt::T("sw.hint",
					 "点按钮立即切换当前作用域画布的场景（Cut）；坞聚焦时数字键 1-9 直切。节目单负责顺序演出，这里负责临场跳转。",
					 "Click a button to cut the scoped canvas to that scene; Ctrl+1-9 cuts directly. Rundown handles sequenced shows, this panel handles live jumps."),
				  this);
	hint->setWordWrap(true);
	hint->setStyleSheet(QStringLiteral("color:#666; font-size:11px;"));
	root->addWidget(hint);

	gridHost = new QWidget(this);
	grid = new QGridLayout(gridHost);
	grid->setContentsMargins(0, 0, 0, 0);
	grid->setSpacing(4);
	root->addWidget(gridHost, 1);
	root->addStretch(0);

	RefreshScenes();
}

std::vector<obs_source_t *> HFRSwitcherPanel::EnumerateScenes() const
{
	std::vector<obs_source_t *> scenes;
	if (canvas) {
		obs_canvas_enum_scenes(canvas, CollectScenesCb, &scenes);
	}
	return scenes;
}

void HFRSwitcherPanel::ClearButtons()
{
	if (!grid) {
		return;
	}
	while (QLayoutItem *item = grid->takeAt(0)) {
		if (QWidget *w = item->widget()) {
			w->deleteLater();
		}
		delete item;
	}
}

void HFRSwitcherPanel::SetCanvas(obs_canvas_t *cv)
{
	canvas = cv;
	RefreshScenes();
}

void HFRSwitcherPanel::RefreshScenes()
{
	ClearButtons();
	if (!canvas) {
		scopeLabel->setText(QStringLiteral("作用域：未选中画布（先在「画布」页选中一行）"));
		return;
	}
	const QString canvasName = QString::fromUtf8(obs_canvas_get_name(canvas));
	scopeLabel->setText(QStringLiteral("作用域：%1").arg(canvasName));

	std::vector<obs_source_t *> scenes = EnumerateScenes();
	obs_source_t *cur = obs_canvas_get_channel(canvas, 0);
	const QString curName = cur ? QString::fromUtf8(obs_source_get_name(cur)) : QString();

	const int kCols = 3;
	for (size_t i = 0; i < scenes.size(); i++) {
		obs_source_t *sc = scenes[i];
		QString name = QString::fromUtf8(obs_source_get_name(sc));
		QPushButton *btn = new QPushButton(name, gridHost);
		btn->setMinimumHeight(34);
		if (name == curName) {
			btn->setText(QStringLiteral("▶ ") + name);
			btn->setStyleSheet(QStringLiteral("font-weight:bold;"));
		}
		const int idx = (int)i;
		connect(btn, &QPushButton::clicked, this, [this, idx]() {
			std::vector<obs_source_t *> list = EnumerateScenes();
			if (idx >= 0 && idx < (int)list.size()) {
				SwitchTo(list[(size_t)idx]);
			}
		});
		grid->addWidget(btn, (int)(i / (size_t)kCols), (int)(i % (size_t)kCols));
	}
	if (scenes.empty()) {
		QLabel *empty = new QLabel(QStringLiteral("该画布还没有场景：到「画布」页点「+ 场景」"), gridHost);
		empty->setWordWrap(true);
		grid->addWidget(empty, 0, 0, 1, kCols);
	}
}

void HFRSwitcherPanel::SwitchTo(obs_source_t *sceneSource)
{
	if (!canvas || !sceneSource) {
		return;
	}
	obs_canvas_set_channel(canvas, 0, sceneSource);
	const QString sceneName = QString::fromUtf8(obs_source_get_name(sceneSource));
	const QString canvasName = QString::fromUtf8(obs_canvas_get_name(canvas));
	if (statusCb) {
		statusCb(QStringLiteral("切换：%1 → %2").arg(canvasName, sceneName));
	}
	RefreshScenes();
}

bool HFRSwitcherPanel::ActivateIndex(int oneBasedIndex)
{
	if (oneBasedIndex < 1) {
		return false;
	}
	std::vector<obs_source_t *> list = EnumerateScenes();
	const size_t idx = (size_t)(oneBasedIndex - 1);
	if (idx >= list.size()) {
		return false;
	}
	SwitchTo(list[idx]);
	return true;
}
