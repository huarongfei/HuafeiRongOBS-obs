#pragma once

#include <QWidget>

#include <functional>
#include <vector>

#include <obs.hpp>

class QLabel;
class QGridLayout;

/* 画布切换台（v1）：把"当前操作画布"作为作用域，按钮/数字键直接切它的场景。
 * 设计见 docs/spec-切换台作用域.md。
 * 生命周期：不持有 canvas 引用；由导播坞在画布增删/选中变化时调用 SetCanvas()。 */
class HFRSwitcherPanel : public QWidget {
public:
	explicit HFRSwitcherPanel(QWidget *parent = nullptr);

	/* 设置作用域画布（传 nullptr = 无作用域） */
	void SetCanvas(obs_canvas_t *cv);
	/* 重新枚举场景并重建按钮 */
	void RefreshScenes();
	/* 数字键 1-9 直切（由坞转发键盘事件） */
	bool ActivateIndex(int oneBasedIndex);

	void SetStatusCallback(std::function<void(const QString &)> cb) { statusCb = std::move(cb); }

private:
	void ClearButtons();
	void SwitchTo(obs_source_t *sceneSource);
	std::vector<obs_source_t *> EnumerateScenes() const;

	obs_canvas_t *canvas = nullptr;
	QLabel *scopeLabel = nullptr;
	QWidget *gridHost = nullptr;
	QGridLayout *grid = nullptr;
	std::function<void(const QString &)> statusCb;
};
