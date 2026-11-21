#include "HFRConsole.hpp"

#include "HFRHotkeyHelp.hpp"
#include "HFRStrings.hpp"

#include "HFRPresenter.hpp"
#include "HFRSlidesSource.hpp"
#include "HFRFontFixDialog.hpp"
#include "HFRPptPlayer.hpp"
#include "HFRSwitcherPanel.hpp"

#include <QAbstractItemModel>
#include <QCheckBox>
#include <QInputDialog>
#include <QMessageBox>
#include <QComboBox>
#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QShortcut>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <obs-module.h>

/* ============================================================
 * 导播坞 UI（单独维护，业务逻辑见 HFRConsole.cpp）
 *
 * 设计原则（人性化）：
 *  - 分页：画布 / 节目单 / 素材 / 输出，避免一长列按钮堆叠
 *  - 每页顶部一句操作提示（先做什么、再做什么）
 *  - 底部常驻状态栏：任何操作都有反馈
 *  - 危险操作（删除/停止/关闭全部）集中在「输出」页，且措辞明确
 * ============================================================ */

static QLabel *HfrHint(QWidget *parent, const QString &text)
{
	QLabel *l = new QLabel(text, parent);
	l->setWordWrap(true);
	l->setStyleSheet(QStringLiteral("color:#666; font-size:11px;"));
	return l;
}

void HFRConsoleDock::BuildUi()
{
	setAcceptDrops(true); /* 支持把素材文件直接拖进导播坞 */
	setMinimumWidth(360);

	QWidget *panel = new QWidget(this);
	QVBoxLayout *root = new QVBoxLayout(panel);
	root->setContentsMargins(6, 6, 6, 6);
	root->setSpacing(6);

	tabs = new QTabWidget(panel);
	tabs->setDocumentMode(true);
	root->addWidget(tabs, 1);

	/* ---------------- 页签 1：画布 ---------------- */
	QWidget *pageCanvas = new QWidget(tabs);
	QVBoxLayout *l1 = new QVBoxLayout(pageCanvas);
	l1->setContentsMargins(4, 6, 4, 4);
	l1->setSpacing(4);
	l1->addWidget(HfrHint(pageCanvas,
			      hfrt::T("hint.canvas",
				      "用法：点选显示器编号行 → 到「输出」页点投影；点选画布行 = 当前操作画布，双击画布行 = 投到主屏。",
				      "Tip: select a monitor row then Project on the Output tab; select a canvas row = active canvas; double-click a canvas = project to main screen.")));
	l1->addWidget(new QLabel(hfrt::T("k5", "显示器（自动编号）", "Monitors (auto-numbered)"), pageCanvas));
	monitorList = new QListWidget(pageCanvas);
	monitorList->setMaximumHeight(92);
	l1->addWidget(monitorList);

	QHBoxLayout *rowCanvasTop = new QHBoxLayout();
	btnRefresh = new QPushButton(hfrt::T("k6", "刷新显示器", "Refresh monitors"), pageCanvas);
	btnNew = new QPushButton(hfrt::T("k7", "+ 新建画布", "New canvas"), pageCanvas);
	btnRename = new QPushButton(hfrt::T("k8", "重命名", "Rename"), pageCanvas);
	btnRemove = new QPushButton(hfrt::T("k9", "删除画布", "Delete canvas"), pageCanvas);
	rowCanvasTop->addWidget(btnRefresh);
	rowCanvasTop->addWidget(btnNew);
	rowCanvasTop->addWidget(btnRename);
	rowCanvasTop->addWidget(btnRemove);
	l1->addLayout(rowCanvasTop);

	/* 多屏布局：命名保存当前投影编排，一键还原 */
	QHBoxLayout *layoutRow = new QHBoxLayout();
	layoutCombo = new QComboBox(pageCanvas);
	layoutCombo->setToolTip(QStringLiteral("已保存的多屏布局（画布→显示器 映射）"));
	btnLayoutSave = new QPushButton(hfrt::T("k10", "存布局", "Save layout"), pageCanvas);
	btnLayoutSave->setToolTip(QStringLiteral("把当前所有投影保存为命名布局"));
	btnLayoutApply = new QPushButton(hfrt::T("k11", "用布局", "Apply layout"), pageCanvas);
	btnLayoutApply->setToolTip(QStringLiteral("关闭现有投影并按所选布局重新投影"));
	btnLayoutDel = new QPushButton(hfrt::T("k12", "删布局", "Delete layout"), pageCanvas);
	layoutRow->addWidget(new QLabel(hfrt::T("k13", "布局：", "Layout:"), pageCanvas));
	layoutRow->addWidget(layoutCombo, 1);
	layoutRow->addWidget(btnLayoutSave);
	layoutRow->addWidget(btnLayoutApply);
	layoutRow->addWidget(btnLayoutDel);
	l1->addLayout(layoutRow);

	l1->addWidget(new QLabel(hfrt::T("k14", "画布（点选行 = 当前操作画布）", "Canvases (selected row = active canvas)"), pageCanvas));
	canvasList = new QListWidget(pageCanvas);
	canvasList->setUniformItemSizes(false);
	l1->addWidget(canvasList, 1);

	l1->addWidget(new QLabel(hfrt::T("k15", "场景（当前画布）", "Scenes (active canvas)"), pageCanvas));
	sceneList = new QListWidget(pageCanvas);
	sceneList->setMaximumHeight(104);
	l1->addWidget(sceneList);

	QHBoxLayout *scRow = new QHBoxLayout();
	btnSceneNew = new QPushButton(hfrt::T("k16", "+ 场景", "+ Scene"), pageCanvas);
	btnSceneUse = new QPushButton(hfrt::T("k17", "切到此场景", "Use scene"), pageCanvas);
	btnSceneDel = new QPushButton(hfrt::T("k18", "删场景", "Delete scene"), pageCanvas);
	btnSceneColor = new QPushButton(hfrt::T("k19", "加色块(演示)", "Add color block (demo)"), pageCanvas);
	scRow->addWidget(btnSceneNew);
	scRow->addWidget(btnSceneUse);
	scRow->addWidget(btnSceneDel);
	scRow->addWidget(btnSceneColor);
	l1->addLayout(scRow);
	tabs->addTab(pageCanvas, hfrt::T("k0", "画布", "Canvas"));

	/* ---------------- 页签 2：节目单 ---------------- */
	QWidget *pageCue = new QWidget(tabs);
	QVBoxLayout *l2 = new QVBoxLayout(pageCue);
	l2->setContentsMargins(4, 6, 4, 4);
	l2->setSpacing(4);
	l2->addWidget(HfrHint(pageCue,
			      hfrt::T("hint.cue",
				      "编排顺序：在「画布」页选好画布 + 场景 → 点「加入当前场景」；演出时用「执行选中 / 上一条 / 下一条」或「自动播放」（按设定秒数推进）。节目单随配置保存，重启自动恢复。",
				      "Order: pick canvas + scene then Add current scene; run with Run selected / Prev / Next or Auto play (interval seconds). The rundown is persisted and restored on restart.")));
	cueList = new QListWidget(pageCue);
	cueList->setDragDropMode(QAbstractItemView::InternalMove);
	cueList->setDefaultDropAction(Qt::MoveAction);
	cueList->setSelectionMode(QAbstractItemView::SingleSelection);
	cueList->setToolTip(QStringLiteral("可直接拖动条目调整顺序；双击=执行；Delete=删除"));
	l2->addWidget(cueList, 1);

	QHBoxLayout *cueRow1 = new QHBoxLayout();
	btnCueAdd = new QPushButton(hfrt::T("k20", "＋ 加入当前场景", "+ Add current scene"), pageCue);
	btnCueAdd->setToolTip(QStringLiteral("把当前 画布+场景 加入节目单；条目可拖拽排序"));
	btnCueDel = new QPushButton(hfrt::T("k21", "删除条目", "Delete cue"), pageCue);
	btnCueUp = new QPushButton(hfrt::T("k22", "↑ 上移", "Move up"), pageCue);
	btnCueDown = new QPushButton(hfrt::T("k23", "↓ 下移", "Move down"), pageCue);
	cueRow1->addWidget(btnCueAdd);
	cueRow1->addWidget(btnCueDel);
	cueRow1->addWidget(btnCueUp);
	cueRow1->addWidget(btnCueDown);
	l2->addLayout(cueRow1);

	QHBoxLayout *cueRow2 = new QHBoxLayout();
	btnCueRun = new QPushButton(hfrt::T("k24", "▶ 执行选中", "Run selected"), pageCue);
	btnCuePrev = new QPushButton(hfrt::T("k25", "◀ 上一条", "Prev cue"), pageCue);
	btnCueNext = new QPushButton(hfrt::T("k26", "下一条 ▶", "Next cue"), pageCue);
	btnCueAuto = new QPushButton(hfrt::T("k27", "▶ 自动播放", "Auto play"), pageCue);
	cueInterval = new QSpinBox(pageCue);
	cueInterval->setRange(1, 600);
	cueInterval->setValue(5);
	cueInterval->setSuffix(QStringLiteral(" 秒"));
	cueInterval->setMaximumWidth(86);
	cueRow2->addWidget(btnCueRun);
	cueRow2->addWidget(btnCuePrev);
	cueRow2->addWidget(btnCueNext);
	cueRow2->addWidget(btnCueAuto);
	cueRow2->addWidget(cueInterval);
	l2->addLayout(cueRow2);

	QHBoxLayout *cueRow3 = new QHBoxLayout();
	btnCueSpecialSlidesNext = new QPushButton(hfrt::T("k28", "＋幻灯片下页", "+ Slide next"), pageCue);
	btnCueSpecialSlidesPrev = new QPushButton(hfrt::T("k29", "＋幻灯片上页", "+ Slide prev"), pageCue);
	btnCueSpecialBlack = new QPushButton(hfrt::T("k30", "＋黑场", "+ Blackout"), pageCue);
	btnCueSpecialRestore = new QPushButton(hfrt::T("k31", "＋恢复画面", "+ Restore"), pageCue);
	btnCueSpecialDelay = new QPushButton(hfrt::T("k32", "＋延时…", "+ Delay..."), pageCue);
	btnCueSpecialDelay->setToolTip(QStringLiteral("插入延时条目：自动播放时等待 N 秒后继续；手动模式按下一步继续"));
	btnCueSpecialScope = new QPushButton(hfrt::T("k33", "＋切操作画布", "+ Switch scope"), pageCue);
	btnCueSpecialScope->setToolTip(QStringLiteral("插入条目：执行时把导播坞的操作画布切到当前选中的画布"));
	btnCueExport = new QPushButton(hfrt::T("k34", "导出", "Export"), pageCue);
	btnCueImport = new QPushButton(hfrt::T("k35", "导入", "Import"), pageCue);
	cueRow3->addWidget(btnCueSpecialSlidesNext);
	cueRow3->addWidget(btnCueSpecialSlidesPrev);
	cueRow3->addWidget(btnCueSpecialBlack);
	cueRow3->addWidget(btnCueSpecialRestore);
	cueRow3->addWidget(btnCueSpecialDelay);
	cueRow3->addWidget(btnCueSpecialScope);
	cueRow3->addStretch(1);
	cueRow3->addWidget(btnCueExport);
	cueRow3->addWidget(btnCueImport);
	l2->addLayout(cueRow3);
	tabs->addTab(pageCue, hfrt::T("k1", "节目单", "Rundown"));

	/* ---------------- 页签 3：素材 ---------------- */
	QWidget *pageMedia = new QWidget(tabs);
	QVBoxLayout *l3 = new QVBoxLayout(pageMedia);
	l3->setContentsMargins(4, 6, 4, 4);
	l3->setSpacing(4);
	l3->addWidget(HfrHint(pageMedia,
			      hfrt::T("hint.media",
				      "导入的图片 / 视频 / 音频会加入「当前选中画布的场景」；视频与音频默认循环播放。「加倒计时」会让你输入分钟数（1–600），在场景中放一个每秒刷新的计时。双击下方条目可定位到对应画布/场景。",
				      "Imported images/videos/audio go to the active canvas scene; video/audio loop by default. Countdown asks minutes (1-600) and adds a per-second timer to the scene. Double-click an item to locate its canvas/scene.")));
	QHBoxLayout *mediaRow = new QHBoxLayout();
	btnMediaAdd = new QPushButton(hfrt::T("k36", "导入素材到当前场景", "Import media to scene"), pageMedia);
	btnCountdown = new QPushButton(hfrt::T("k37", "加倒计时…", "+ Countdown..."), pageMedia);
	mediaRow->addWidget(btnMediaAdd);
	mediaRow->addWidget(btnCountdown);
	l3->addLayout(mediaRow);
	l3->addWidget(new QLabel(hfrt::T("k38", "本次已导入（双击条目 = 定位到对应画布/场景）", "Imported (double-click to locate)"), pageMedia));
	mediaList = new QListWidget(pageMedia);
	l3->addWidget(mediaList, 1);
	tabs->addTab(pageMedia, hfrt::T("k2", "素材", "Media"));

	/* ---------------- 页签 4：输出 ---------------- */
	QWidget *pageOut = new QWidget(tabs);
	QVBoxLayout *l4 = new QVBoxLayout(pageOut);
	l4->setContentsMargins(4, 6, 4, 4);
	l4->setSpacing(5);
	l4->addWidget(HfrHint(pageOut,
			      hfrt::T("hint.out",
				      "投影与录制都针对「当前操作画布」；停止类按钮会立即生效。异形屏/非固定分辨率：画布分辨率=拼接墙分辨率，「拼接投影…」按分块铺到多块物理屏；普通投影自动等比缩放居中。本产品专注本地多输出，不包含对网推流。",
				      "Projection and recording target the active canvas; stop buttons apply immediately. Irregular/non-standard resolution: canvas resolution = wall resolution; Splice projection maps tiles onto physical screens; normal projection is letterboxed. Local multi-output only (no network streaming).")));

	l4->addWidget(new QLabel(hfrt::T("k39", "投影 / 安全区", "Projection / Safe area"), pageOut));
	QHBoxLayout *actRow = new QHBoxLayout();
	btnProject = new QPushButton(hfrt::T("k40", "▶ 投影到选中屏", "Project to selected screen"), pageOut);
	btnSafeArea = new QPushButton(hfrt::T("k41", "安全区开关", "Safe area"), pageOut);
	btnStopOne = new QPushButton(hfrt::T("k42", "■ 停本画布投影", "Stop canvas projection"), pageOut);
	btnStopAll = new QPushButton(hfrt::T("k43", "■ 关闭全部投影", "Close all projections"), pageOut);
	btnTest = new QPushButton(hfrt::T("k44", "填测试色", "Fill test color"), pageOut);
	btnSplice = new QPushButton(hfrt::T("k45", "拼接投影…", "Splice projection..."), pageOut);
	btnBlackout = new QPushButton(hfrt::T("k46", "■ 黑场", "Blackout"), pageOut);
	btnBlackout->setToolTip(QStringLiteral("一键把所有画布输出切到黑场（再点恢复原场景），演出应急/换场常用"));
	btnSplice->setToolTip(QStringLiteral("异形屏拼接：把画布（=自定义分辨率的墙）按分块铺到多块物理屏"));
	actRow->addWidget(btnProject);
	actRow->addWidget(btnSplice);
	actRow->addWidget(btnBlackout);
	actRow->addWidget(btnSafeArea);
	actRow->addWidget(btnStopOne);
	actRow->addWidget(btnStopAll);
	actRow->addWidget(btnTest);
	l4->addLayout(actRow);

	l4->addWidget(new QLabel(hfrt::T("k48", "录制（绑当前画布）", "Recording (bound to canvas)"), pageOut));
	QHBoxLayout *outRow = new QHBoxLayout();
	encCombo = new QComboBox(pageOut);
	{
		const char *id = nullptr;
		for (size_t i = 0; obs_enum_encoder_types(i, &id); i++) {
			if (!id) {
				continue;
			}
			const QString sid = QString::fromUtf8(id);
			if (sid.contains(QStringLiteral("264")) || sid == QStringLiteral("jim_nvenc")) {
				encCombo->addItem(sid, sid);
			}
		}
		const int def = encCombo->findData(QStringLiteral("obs_x264"));
		if (def >= 0) {
			encCombo->setCurrentIndex(def);
		}
	}
	btnRecStart = new QPushButton(hfrt::T("k49", "● 录制该画布", "Record canvas"), pageOut);
	btnRecStop = new QPushButton(hfrt::T("k50", "■ 停录制", "Stop recording"), pageOut);
	btnRecStopAll = new QPushButton(hfrt::T("k51", "■ 全部停录", "Stop all recordings"), pageOut);
	outRow->addWidget(encCombo);
	outRow->addWidget(btnRecStart);
	outRow->addWidget(btnRecStop);
	outRow->addWidget(btnRecStopAll);

	/* 音频双轨：第 1 轨现场混音，第 2 轨解说源（解说只入录制，不出现场） */
	QHBoxLayout *audioRow = new QHBoxLayout();
	chkDualTrack = new QCheckBox(hfrt::T("k52", "双轨录制", "Dual-track"), pageOut);
	chkDualTrack->setToolTip(QStringLiteral("第1轨=现场混音；第2轨=解说源（只录进文件，不进入现场/第1轨）"));
	commentatorCombo = new QComboBox(pageOut);
	commentatorCombo->setMinimumWidth(140);
	commentatorCombo->setToolTip(QStringLiteral("解说音源（麦克风等音频源）"));
	btnCommentatorApply = new QPushButton(hfrt::T("k53", "应用解说源", "Apply commentator"), pageOut);
	btnAudioRoute = new QPushButton(hfrt::T("k54", "音频路由…", "Audio routing..."), pageOut);
	btnAudioRoute->setToolTip(QStringLiteral("为每个音频源勾选录制音轨（1=现场，2..6=附加如解说）；录制时自动生成多轨"));
	audioRow->addWidget(chkDualTrack);
	audioRow->addWidget(commentatorCombo, 1);
	audioRow->addWidget(btnCommentatorApply);
	audioRow->addWidget(btnAudioRoute);
	l4->addLayout(audioRow);
	l4->addLayout(outRow);

	l4->addWidget(new QLabel(hfrt::T("k55", "PPT / 幻灯片（自研引擎）", "PPT / Slides (built-in engine)"), pageOut));
	QHBoxLayout *pptRow = new QHBoxLayout();
	btnPptOpen = new QPushButton(hfrt::T("k56", "打开 PPT", "Open PPT"), pageOut);
	btnPptPrev = new QPushButton(hfrt::T("k57", "PPT 上一页", "PPT prev"), pageOut);
	btnPptNext = new QPushButton(hfrt::T("k58", "PPT 下一页", "PPT next"), pageOut);
	btnPresenter = new QPushButton(hfrt::T("k59", "讲者视图", "Presenter"), pageOut);
	btnPptPlayer = new QPushButton(hfrt::T("k60", "独立播放 UI", "Standalone player"), pageOut);
	btnPptPlayer->setToolTip(QStringLiteral("独立 PPT 播放窗（不依赖画布/来源）：全屏播放 + 控制台（翻页/动画/自动播放/黑屏/选屏）"));
	pptRow->addWidget(btnPptOpen);
	pptRow->addWidget(btnPptPrev);
	pptRow->addWidget(btnPptNext);
	pptRow->addWidget(btnPresenter);
	pptRow->addWidget(btnPptPlayer);
	l4->addLayout(pptRow);

	QHBoxLayout *slidesRow = new QHBoxLayout();
	btnSlidesOpen = new QPushButton(hfrt::T("k61", "打开幻灯片", "Open slides"), pageOut);
	btnSlidesPrev = new QPushButton(hfrt::T("k62", "◀ 页", "Prev page"), pageOut);
	btnSlidesNext = new QPushButton(hfrt::T("k63", "页 ▶", "Next page"), pageOut);
	btnAnimNext = new QPushButton(hfrt::T("k64", "▶ 动画", "Anim next"), pageOut);
	btnAnimPrev = new QPushButton(hfrt::T("k65", "◀ 动画", "Anim prev"), pageOut);
	btnFontFix = new QPushButton(hfrt::T("k66", "字体修复…", "Fix fonts..."), pageOut);
	btnFontFix->setToolTip(QStringLiteral("缺字体一键替换：列出缺失字体并选择本机替代字体，写入来源设置并重建"));
	slidesRow->addWidget(btnSlidesOpen);
	slidesRow->addWidget(btnSlidesPrev);
	slidesRow->addWidget(btnSlidesNext);
	slidesRow->addWidget(btnAnimNext);
	slidesRow->addWidget(btnAnimPrev);
	slidesRow->addWidget(btnFontFix);
	l4->addLayout(slidesRow);

	/* 界面语言（重启生效；主界面关键文案双语，专业对话框暂中文） */
	QHBoxLayout *langRow = new QHBoxLayout();
	langRow->addWidget(new QLabel(QStringLiteral("界面语言 / Language："), pageOut));
	langCombo = new QComboBox(pageOut);
	langCombo->addItem(QStringLiteral("中文"), 0);
	langCombo->addItem(QStringLiteral("English (main UI)"), 1);
	langCombo->setCurrentIndex(hfrt::GetLang() == hfrt::Lang::En ? 1 : 0);
	langCombo->setToolTip(QStringLiteral("主界面关键文案中/英切换（重启后生效）"));
	langRow->addWidget(langCombo);
	btnDiag = new QPushButton(QStringLiteral("导出诊断…"), pageOut);
	btnDiag->setToolTip(QStringLiteral("导出环境/画布/拼接/投影/录制/节目单/源清单与日志尾部，便于远程排障"));
	langRow->addWidget(btnDiag);
	btnDefaultHotkeys = new QPushButton(QStringLiteral("应用默认热键"), pageOut);
	btnDefaultHotkeys->setToolTip(QStringLiteral("绑定 Ctrl+Alt+1..9：下一条/上一条/执行/自动/黑场/投影/停投影/录制/停录"));
	langRow->addWidget(btnDefaultHotkeys);
	btnHotkeyHelp = new QPushButton(QStringLiteral("快捷键帮助"), pageOut);
	btnHotkeyHelp->setToolTip(QStringLiteral("查看前端热键名、画布级动态热键与界面快捷键"));
	langRow->addWidget(btnHotkeyHelp);
	langRow->addStretch(1);
	l4->addLayout(langRow);
	l4->addStretch(1);
	tabs->addTab(pageOut, hfrt::T("k3", "输出", "Output"));

	/* ---------------- 页签 5：切换台（作用域 = 当前操作画布） ---------------- */
	switcher = new HFRSwitcherPanel(tabs);
	switcher->SetStatusCallback([this](const QString &msg) { SetStatus(msg); });
	tabs->addTab(switcher, hfrt::T("k4", "切换", "Switcher"));

	/* 关键操作提示（悬停可见，降低误操作） */
	btnProject->setToolTip(QStringLiteral("把当前画布全屏投影到「画布」页选中的显示器编号"));
	btnStopOne->setToolTip(QStringLiteral("只停止当前画布的投影"));
	btnStopAll->setToolTip(QStringLiteral("关闭全部投影窗口"));
	btnSafeArea->setToolTip(QStringLiteral("在投影上叠加 90% / 80% 安全区参考线"));
	btnRecStart->setToolTip(QStringLiteral("按选中编码器把当前画布录制为独立 MKV"));
	btnCueNext->setToolTip(QStringLiteral("执行节目单下一条（PageDown）"));
	btnCuePrev->setToolTip(QStringLiteral("回到节目单上一条（PageUp）"));
	btnCueAuto->setToolTip(QStringLiteral("按设定秒数自动推进；再次点击停止"));
	btnMediaAdd->setToolTip(QStringLiteral("也可直接把文件拖到本窗口导入"));
	btnCountdown->setToolTip(QStringLiteral("点击后输入分钟数（1–600），在当前场景放一个每秒刷新的倒计时"));
	btnCueExport->setToolTip(QStringLiteral("把节目单保存为 JSON：[{\"canvas\":...,\"scene\":...}]"));
	btnCueImport->setToolTip(QStringLiteral("从 JSON 导入节目单，确认后替换当前节目单"));
	btnSlidesOpen->setToolTip(QStringLiteral("载入 pptx 到「幻灯片（自研引擎）」来源"));
	btnAnimNext->setToolTip(QStringLiteral("推进本页的下一步动画"));
	btnPresenter->setToolTip(QStringLiteral("打开讲者视图：当前页/下一页/备注"));

	/* 底部常驻状态栏 */
	status = new QLabel(hfrt::T("k67", "就绪", "Ready"), panel);
	status->setWordWrap(true);
	status->setMinimumHeight(34);
	status->setStyleSheet(QStringLiteral("color:#222; font-size:12px;"));
	root->addWidget(status);

	setWidget(panel);
}

void HFRConsoleDock::WireSignals()
{
	/* 画布 / 显示器 */
	connect(btnRefresh, &QPushButton::clicked, this, [this]() {
		RefreshMonitors();
		RefreshCanvasList();
	});
	connect(btnLayoutSave, &QPushButton::clicked, this, [this]() { SaveCurrentLayout(); });
	connect(btnLayoutApply, &QPushButton::clicked, this, [this]() { ApplySelectedLayout(); });
	connect(btnLayoutDel, &QPushButton::clicked, this, [this]() { DeleteSelectedLayout(); });

	connect(btnNew, &QPushButton::clicked, this, [this]() { CreateCanvas(); });
	connect(btnRemove, &QPushButton::clicked, this, [this]() { RemoveSelectedCanvas(); });
	connect(btnRename, &QPushButton::clicked, this, [this]() { RenameSelectedCanvas(); });
	connect(canvasList, &QListWidget::currentRowChanged, this, [this](int) {
		UpdateSceneListForSelection();
		if (switcher) {
			switcher->SetCanvas(CanvasFromRow(canvasList->currentRow()));
		}
	});
	/* 页签记忆：切换页签即写入 profile 状态文件（载入期间由 loadingState 拦截） */
	connect(tabs, &QTabWidget::currentChanged, this, [this](int) { SaveState(); });
	connect(canvasList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
		const int row = canvasList->row(item);
		obs_canvas_t *cv = CanvasFromRow(row);
		if (!cv) {
			return;
		}
		const auto screens = QGuiApplication::screens();
		int primary = 0;
		for (int i = 0; i < screens.size(); i++) {
			if (screens.at(i) == QGuiApplication::primaryScreen()) {
				primary = i;
				break;
			}
		}
		ProjectCanvasToMonitor(cv, primary);
	});

	/* 场景 */
	connect(btnSceneNew, &QPushButton::clicked, this, [this]() { CreateSceneForSelected(); });
	connect(btnSceneUse, &QPushButton::clicked, this, [this]() { SwitchToSelectedScene(); });
	connect(btnSceneDel, &QPushButton::clicked, this, [this]() { DeleteSelectedScene(); });
	connect(btnSceneColor, &QPushButton::clicked, this, [this]() { FillColorIntoSelectedScene(); });

	/* 节目单 */
	connect(btnCueAdd, &QPushButton::clicked, this, [this]() { AddCurrentSceneToRundown(); });
	connect(btnCueDel, &QPushButton::clicked, this, [this]() { RemoveSelectedCue(); });
	connect(btnCueUp, &QPushButton::clicked, this, [this]() { MoveCue(-1); });
	connect(btnCueDown, &QPushButton::clicked, this, [this]() { MoveCue(1); });
	connect(btnCueRun, &QPushButton::clicked, this, [this]() { ExecuteCue(cueList->currentRow()); });
	connect(btnCuePrev, &QPushButton::clicked, this, [this]() { PrevCue(); });
	connect(btnCueNext, &QPushButton::clicked, this, [this]() { NextCue(); });
	connect(btnCueAuto, &QPushButton::clicked, this, [this]() { ToggleAutoPlay(); });
	connect(btnCueSpecialSlidesNext, &QPushButton::clicked, this, [this]() { AddSpecialCue(1); });
	connect(btnCueSpecialSlidesPrev, &QPushButton::clicked, this, [this]() { AddSpecialCue(2); });
	connect(btnCueSpecialBlack, &QPushButton::clicked, this, [this]() { AddSpecialCue(3); });
	connect(btnCueSpecialRestore, &QPushButton::clicked, this, [this]() { AddSpecialCue(4); });
	connect(btnCueSpecialDelay, &QPushButton::clicked, this, [this]() {
		bool ok = false;
		const int sec = QInputDialog::getInt(this, QStringLiteral("延时条目"), QStringLiteral("等待秒数"),
						     5, 1, 3600, 1, &ok);
		if (ok) {
			AddDelayCue(sec);
		}
	});
	connect(btnCueSpecialScope, &QPushButton::clicked, this, [this]() { AddScopeCue(); });
	connect(btnCueExport, &QPushButton::clicked, this, [this]() { ExportRundown(); });
	connect(btnCueImport, &QPushButton::clicked, this, [this]() { ImportRundown(); });
	/* 拖拽排序：按模型移动结果重排 cues（延迟刷新，避免与列表内部移动冲突） */
	connect(cueList->model(), &QAbstractItemModel::rowsMoved, this,
		[this](const QModelIndex &, int start, int end, const QModelIndex &, int destRow) {
			if (start < 0 || end < start || end >= cues.size()) {
				return;
			}
			const int count = end - start + 1;
			const int curBefore = cueIndex;
			QVector<HfrCue> moved;
			for (int i = 0; i < count; i++) {
				moved.push_back(cues.takeAt(start));
			}
			int insertAt = destRow;
			if (destRow > start) {
				insertAt -= count;
			}
			insertAt = qBound(0, insertAt, cues.size());
			for (int i = 0; i < moved.size(); i++) {
				cues.insert(insertAt + i, moved.at(i));
			}
			/* 当前执行位置跟随重排 */
			if (curBefore >= 0) {
				if (curBefore >= start && curBefore <= end) {
					cueIndex = insertAt + (curBefore - start);
				} else if (destRow > start) {
					if (curBefore > end && curBefore < destRow) {
						cueIndex = curBefore - count;
					}
				} else if (curBefore >= destRow && curBefore < start) {
					cueIndex = curBefore + count;
				}
			}
			QTimer::singleShot(0, this, [this]() {
				RefreshCueList();
				SaveState();
			});
		});
	connect(cueList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
		ExecuteCue(cueList->row(item));
	});
	cueTimer = new QTimer(this);
	connect(cueTimer, &QTimer::timeout, this, [this]() {
		if (cueIndex + 1 < cues.size()) {
			ExecuteCue(cueIndex + 1);
		} else {
			cueTimer->stop();
			cueAutoPlaying = false;
			btnCueAuto->setText(hfrt::T("k27", "▶ 自动播放", "Auto play"));
			status->setText(QStringLiteral("节目单已播完（%1 条）").arg(cues.size()));
		}
	});

	/* 素材 / 倒计时 */
	connect(btnMediaAdd, &QPushButton::clicked, this, [this]() { ImportMediaIntoScene(); });
	connect(btnCountdown, &QPushButton::clicked, this, [this]() { AddCountdownOverlay(); });
	connect(mediaList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
		LocateMediaAt(mediaList->row(item));
	});
	countdownTimer = new QTimer(this);
	connect(countdownTimer, &QTimer::timeout, this, [this]() {
		const qint64 now = QDateTime::currentMSecsSinceEpoch();
		for (auto it = countdowns.begin(); it != countdowns.end();) {
			const qint64 remain = it.value() - now;
			QString text;
			if (remain <= 0) {
				text = QStringLiteral("00:00");
			} else {
				const int totalSec = (int)(remain / 1000);
				text = QStringLiteral("%1:%2")
					       .arg(totalSec / 60, 2, 10, QLatin1Char('0'))
					       .arg(totalSec % 60, 2, 10, QLatin1Char('0'));
			}
			obs_data_t *s = obs_data_create();
			obs_data_set_string(s, "text", text.toUtf8().constData());
			obs_source_update(it.key(), s);
			obs_data_release(s);
			if (remain <= -5000) {
				obs_source_release(it.key());
				it = countdowns.erase(it);
			} else {
				++it;
			}
		}
	});
	countdownTimer->start(1000);

	/* 快捷键：导播坞获得焦点时 PageDown/PageUp 走节目单（演出常用） */
	/* 切换台：Ctrl+1..9 直切当前作用域画布的第 N 个场景（演出常用，避开输入框冲突） */
	for (int i = 1; i <= 9; i++) {
		auto *sc = new QShortcut(QKeySequence(Qt::CTRL | (Qt::Key_0 + i)), this);
		sc->setContext(Qt::WidgetWithChildrenShortcut);
		connect(sc, &QShortcut::activated, this, [this, i]() {
			if (!switcher) {
				return;
			}
			if (!switcher->ActivateIndex(i)) {
				SetStatus(QStringLiteral("切换台：没有第 %1 个场景").arg(i), 1);
			}
		});
	}

	auto *scNext = new QShortcut(QKeySequence(Qt::Key_PageDown), this);
	scNext->setContext(Qt::WidgetWithChildrenShortcut);
	connect(scNext, &QShortcut::activated, this, [this]() { NextCue(); });
	auto *scPrev = new QShortcut(QKeySequence(Qt::Key_PageUp), this);
	scPrev->setContext(Qt::WidgetWithChildrenShortcut);
	connect(scPrev, &QShortcut::activated, this, [this]() { PrevCue(); });

	/* 输出：投影 / 录制 / PPT / 幻灯片 */
	connect(btnTest, &QPushButton::clicked, this, [this]() { SetTestColorOnSelected(); });
	connect(btnSafeArea, &QPushButton::clicked, this, [this]() { ToggleSafeAreaOnSelected(); });
	connect(btnProject, &QPushButton::clicked, this, [this]() { ProjectSelected(); });
	connect(btnSplice, &QPushButton::clicked, this, [this]() { OpenSpliceDialog(); });
	connect(btnBlackout, &QPushButton::clicked, this, [this]() {
		ToggleBlackout();
		btnBlackout->setText(blackoutActive ? hfrt::T("k47", "恢复画面", "Restore") : hfrt::T("k46", "■ 黑场", "Blackout"));
	});
	connect(btnPptPlayer, &QPushButton::clicked, this, []() { HfrOpenPptPlayerUi(); });
	connect(btnStopOne, &QPushButton::clicked, this, [this]() { StopProjectionOfSelected(); });
	connect(btnStopAll, &QPushButton::clicked, this, [this]() { CloseAllProjectors(); });
	connect(chkDualTrack, &QCheckBox::toggled, this, [this](bool on) {
		if (on) {
			RefreshCommentatorCombo();
			SetStatus(QStringLiteral("双轨录制：请选择解说音源并点「应用解说源」"));
		} else {
			RestoreCommentatorMixers();
			SetStatus(QStringLiteral("双轨录制：已关闭（解说源混音已恢复）"));
		}
	});
	connect(btnCommentatorApply, &QPushButton::clicked, this, [this]() { ApplyCommentatorSource(); });
	connect(btnAudioRoute, &QPushButton::clicked, this, [this]() { OpenAudioRouting(); });
	connect(btnDiag, &QPushButton::clicked, this, [this]() { ExportDiagnostics(); });
	connect(btnDefaultHotkeys, &QPushButton::clicked, this, [this]() {
		const auto ans = QMessageBox::question(
			this, QStringLiteral("应用默认热键"),
			QStringLiteral("将把以下热键覆盖为默认值：\nCtrl+Alt+1..9（下一条/上一条/执行/自动/黑场/投影/停投影/录制/停录）\n\n继续？"),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
		if (ans == QMessageBox::Yes) {
			ApplyDefaultHotkeys();
		}
	});
	connect(btnHotkeyHelp, &QPushButton::clicked, this, [this]() {
		HFRHotkeyHelpDialog dlg(this);
		dlg.exec();
	});
	connect(langCombo, QOverload<int>::of(&QComboBox::activated), this, [this](int idx) {
		hfrt::SetLang(idx == 1 ? hfrt::Lang::En : hfrt::Lang::Zh);
		SetStatus(QStringLiteral("界面语言已保存，重启 OBS 后生效"));
	});
	connect(btnRecStart, &QPushButton::clicked, this, [this]() { StartRecordingSelected(); });
	connect(btnRecStop, &QPushButton::clicked, this, [this]() { StopRecordingSelected(); });
	connect(btnRecStopAll, &QPushButton::clicked, this, [this]() {
		StopAllRecordings();
		RefreshCanvasList();
		status->setText(QStringLiteral("已停止全部画布录制"));
	});
	connect(btnPptOpen, &QPushButton::clicked, this, [this]() { PptOpenTest(); });
	connect(btnPptPrev, &QPushButton::clicked, this, [this]() { PptPrev(); });
	connect(btnPptNext, &QPushButton::clicked, this, [this]() { PptNext(); });
	connect(btnFontFix, &QPushButton::clicked, this, [this]() {
		obs_source_t *src = HfrFindFirstSlidesSource();
		if (!src) {
			SetStatus(QStringLiteral("没有幻灯片来源：先在场景里添加「幻灯片（自研引擎）」并载入 pptx"), 1);
			return;
		}
		HFRFontFixDialog dlg(src, this);
		dlg.exec();
		SetStatus(QStringLiteral("字体替换已写入来源（若有选择），页面已重建"));
	});
	connect(btnPresenter, &QPushButton::clicked, this, []() { HfrOpenPresenterWindow(); });

	connect(btnSlidesOpen, &QPushButton::clicked, this, [this]() {
		obs_source_t *src = HfrFindFirstSlidesSource();
		if (!src) {
			SetStatus(QStringLiteral("请先添加来源：幻灯片（自研引擎）"), 1);
			return;
		}
		const QString file = QFileDialog::getOpenFileName(this, QStringLiteral("选择演示文稿"), QString(),
								 QStringLiteral("演示文稿 (*.pptx *.ppt *.odp)"));
		if (file.isEmpty()) {
			return;
		}
		HfrSlidesSetFile(src, file);
		status->setText(QStringLiteral("幻灯片已载入：%1（共 %2 页）")
					.arg(QFileInfo(file).fileName())
					.arg(HfrSlidesPageCount(src)));
	});
	connect(btnSlidesPrev, &QPushButton::clicked, this, [this]() {
		obs_source_t *src = HfrFindFirstSlidesSource();
		if (!src) {
			return;
		}
		const int cur = HfrSlidesPage(src);
		if (cur > 0) {
			HfrSlidesSetPage(src, cur - 1);
		}
		status->setText(QStringLiteral("幻灯片：第 %1/%2 页").arg(cur).arg(HfrSlidesPageCount(src)));
	});
	connect(btnSlidesNext, &QPushButton::clicked, this, [this]() {
		obs_source_t *src = HfrFindFirstSlidesSource();
		if (!src) {
			return;
		}
		const int cur = HfrSlidesPage(src);
		const int total = HfrSlidesPageCount(src);
		if (cur + 1 < total) {
			HfrSlidesSetPage(src, cur + 1);
		}
		status->setText(QStringLiteral("幻灯片：第 %1/%2 页").arg(cur + 2).arg(total));
	});
	connect(btnAnimNext, &QPushButton::clicked, this, [this]() {
		obs_source_t *src = HfrFindFirstSlidesSource();
		if (!src) {
			return;
		}
		const int total = HfrSlidesAnimStepCount(src);
		if (HfrSlidesNextAnim(src)) {
			status->setText(QStringLiteral("动画：第 %1/%2 步").arg(HfrSlidesAnimStep(src)).arg(total));
		} else {
			status->setText(QStringLiteral("本页动画已播完（%1 步）").arg(total));
		}
	});
	connect(btnAnimPrev, &QPushButton::clicked, this, [this]() {
		obs_source_t *src = HfrFindFirstSlidesSource();
		if (!src) {
			return;
		}
		if (HfrSlidesPrevAnim(src)) {
			status->setText(QStringLiteral("动画回退到第 %1 步").arg(HfrSlidesAnimStep(src)));
		}
	});
}
