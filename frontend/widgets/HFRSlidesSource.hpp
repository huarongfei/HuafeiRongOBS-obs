#pragma once
/* HFRSlidesSource —— 自研 PPT 引擎的 OBS 来源
 * 渲染线程只做纹理上传与绘制；解析/光栅化在 UI 线程完成（见 ADR-005 教训）。 */
#include <obs.hpp>

#include <QString>
#include <string>
#include <vector>

#include "HFRDeck.hpp"
#include "HFRDeckRender.hpp"

struct gs_texture;

struct HfrSlidesGpuItem {
	hfr::ItemKind kind = hfr::ItemKind::Rect;
	int x = 0, y = 0, w = 0, h = 0;
	int rot60k = 0;
	int shapeIndex = -1;
	/* 每帧动画求值结果 */
	float alpha = 1.0f;
	float dx = 0.0f;
	float dy = 0.0f;
	float scale = 1.0f;
	gs_texture_t *tex = nullptr;   /* Rect/Text/Picture 都走纹理 */
	std::string tmpPath;           /* Picture 的临时文件（供 gs_texture_create_from_file） */
};

struct HfrSlidesCtx {
	QString file;
	int page = 0;
	int width = 1920, height = 1080;
	hfr::Deck deck;
	hfr::RenderSlide slide;
	bool loaded = false;
	bool needBuild = false;      /* UI 线程：需要重新解析/光栅化 */
	bool needUpload = false;     /* 图形线程：需要上传纹理 */
	std::wstring lastError;
	std::vector<HfrSlidesGpuItem> gpu;

	/* —— 动画时间轴 —— */
	int animStep = 0;            /* 已完成的动画步数 */
	bool animating = false;
	uint64_t animStartNs = 0;
	/* 一步内的最大时长（毫秒），用于判断动画是否结束 */
	double animDurationMs = 0.0;

	/* —— 页间切换 —— */
	std::vector<HfrSlidesGpuItem> gpuOld; /* 上一页纹理（切换期间保留） */
	bool transActive = false;
	uint64_t transStartNs = 0;
	double transDurMs = 0.0;
	int transKind = 0; /* 0=无 1=淡入淡出 2=推进 3=擦除/覆盖 */
	int transDirX = 0, transDirY = 0;
	bool needDestroyOld = false;
};

/* 注册来源类型（来源→+ 里可见） */
bool HfrRegisterSlidesSource();

/* 供控制台/讲者视图使用：设置文件与页码（UI 线程） */
void HfrSlidesSetFile(obs_source_t *src, const QString &path);
void HfrSlidesSetPage(obs_source_t *src, int page);
int HfrSlidesPage(obs_source_t *src);
int HfrSlidesPageCount(obs_source_t *src);
obs_source_t *HfrFindFirstSlidesSource();

/* 当前页备注文本（讲者视图用） */
std::wstring HfrSlidesNotes(obs_source_t *src);

/* CPU 渲染某页为 BGRA（讲者视图预览用；w/h 为目标尺寸） */
bool HfrSlidesRenderPreview(obs_source_t *src, int page, int w, int h, std::vector<uint8_t> &bgraOut);

/* 动画控制（UI 线程调用） */
int HfrSlidesAnimStep(obs_source_t *src);      /* 已完成的步数 */
int HfrSlidesAnimStepCount(obs_source_t *src); /* 总步数 */
bool HfrSlidesNextAnim(obs_source_t *src);     /* 推进一步动画（无则返回 false） */
bool HfrSlidesPrevAnim(obs_source_t *src);
void HfrSlidesResetAnim(obs_source_t *src);
