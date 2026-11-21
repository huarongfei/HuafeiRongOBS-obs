#include "HFRHotkeyHelp.hpp"

#include "HFRStrings.hpp"

#include <QDialogButtonBox>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>

static const char *kHtml =
	"<h3>HFR 热键 / 远程触发</h3>"
	"<p>前端热键可在 <b>设置 → 热键</b> 绑定；也可用 obs-websocket 的 "
	"<code>TriggerHotkeyByName</code> 按名称触发（无需绑定）。</p>"
	"<h4>节目单 / 全局</h4>"
	"<table cellspacing=4>"
	"<tr><td><code>hfr.cue_next</code></td><td>节目单下一条</td></tr>"
	"<tr><td><code>hfr.cue_prev</code></td><td>节目单上一条</td></tr>"
	"<tr><td><code>hfr.cue_run_current</code></td><td>执行当前节目单条目</td></tr>"
	"<tr><td><code>hfr.cue_auto_toggle</code></td><td>自动播放 开/关</td></tr>"
	"<tr><td><code>hfr.blackout_toggle</code></td><td>黑场 开/关（所有画布）</td></tr>"
	"<tr><td><code>hfr.project_primary</code></td><td>当前画布投影到主屏</td></tr>"
	"<tr><td><code>hfr.stop_all_projections</code></td><td>关闭全部投影</td></tr>"
	"<tr><td><code>hfr.rec_start</code> / <code>hfr.rec_stop</code></td><td>录制/停止录制当前画布</td></tr>"
	"</table>"
	"<h4>画布级（自动注册，最多 4 画布 × 4 屏 / 4 场景）</h4>"
	"<table cellspacing=4>"
	"<tr><td><code>hfr.canvas.{画布行}.project_screen.{屏号}</code></td><td>画布 → 投影到指定屏</td></tr>"
	"<tr><td><code>hfr.canvas.{画布行}.scene.{场景序号}</code></td><td>画布 → 切到第 N 个场景</td></tr>"
	"</table>"
	"<p>例：<code>hfr.canvas.0.project_screen.1</code> = 第 1 个画布投到屏 1；"
	"<code>hfr.canvas.0.scene.2</code> = 第 1 个画布切到第 2 个场景。</p>"
	"<h4>界面快捷键</h4>"
	"<table cellspacing=4>"
	"<tr><td><b>Ctrl+1..9</b></td><td>切换台：直接切到第 N 个场景</td></tr>"
	"<tr><td><b>PageUp / PageDown</b></td><td>节目单上一条 / 下一条</td></tr>"
	"<tr><td><b>← / → / PgUp / PgDn / Space</b></td><td>独立播放器：翻页与动画</td></tr>"
	"<tr><td><b>B</b> / <b>N</b> / <b>Esc</b></td><td>播放器：黑屏 / 备注浮层 / 退出全屏</td></tr>"
	"</table>"
	"<p>详见 <code>docs/远程控制热键.md</code>。</p>";

HFRHotkeyHelpDialog::HFRHotkeyHelpDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(hfrt::T("help.hotkeys.title", "快捷键 / 远程热键帮助", "Hotkeys / Remote Triggers"));
	resize(620, 560);
	QVBoxLayout *root = new QVBoxLayout(this);
	QTextBrowser *view = new QTextBrowser(this);
	view->setOpenExternalLinks(true);
	view->setHtml(QString::fromUtf8(kHtml));
	root->addWidget(view, 1);
	QDialogButtonBox *box = new QDialogButtonBox(QDialogButtonBox::Close, this);
	box->button(QDialogButtonBox::Close)->setText(hfrt::T("btn.close", "关闭", "Close"));
	root->addWidget(box);
	connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
}
