#include "HFRSlidesSource.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>

#include "HFRPpt.hpp" /* 复用 HfrPptTestOutputDir()/缓存目录 */

#include <obs-module.h>
#include <util/platform.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <cmath>
#include <cstring>

/* 前置声明（属性面板按钮与换页逻辑会用到） */
static int step_count(HfrSlidesCtx *ctx);
static void start_step_anim(HfrSlidesCtx *ctx);
static void start_transition(HfrSlidesCtx *ctx);
static void destroy_items(std::vector<HfrSlidesGpuItem> &items);

/* ============================ 来源实现 ============================ */

static const char *hfr_slides_get_name(void *)
{
	return "幻灯片（自研引擎）";
}

static void hfr_slides_destroy_gpu(HfrSlidesCtx *ctx)
{
	if (!ctx) {
		return;
	}
	obs_enter_graphics();
	for (HfrSlidesGpuItem &it : ctx->gpu) {
		if (it.tex) {
			gs_texture_destroy(it.tex);
			it.tex = nullptr;
		}
	}
	obs_leave_graphics();
	ctx->gpu.clear();
}

/* UI 线程：解析 + CPU 光栅化 */
static void hfr_slides_reset_timeline(HfrSlidesCtx *ctx)
{
	if (ctx) {
		ctx->animStep = 0;
		ctx->animating = false;
		ctx->animStartNs = 0;
		ctx->animDurationMs = 0.0;
	}
}

static void hfr_slides_rebuild(HfrSlidesCtx *ctx)
{
	if (!ctx) {
		return;
	}
	ctx->lastError.clear();
	ctx->deck = hfr::Deck();
	ctx->slide.items.clear();
	ctx->loaded = false;

	if (ctx->file.isEmpty()) {
		ctx->lastError = L"未选择文件";
		return;
	}
	std::wstring err;
	if (!hfr::LoadDeck(ctx->file.toStdWString(), ctx->deck, err)) {
		ctx->lastError = err;
		blog(LOG_WARNING, "[HFR-SLIDES] 解析失败：%ls", err.c_str());
		return;
	}
	if (ctx->page < 0 || ctx->page >= (int)ctx->deck.slides.size()) {
		ctx->page = 0;
	}
	if (!hfr::BuildSlide(ctx->deck, ctx->page, ctx->width, ctx->height, ctx->slide, err)) {
		ctx->lastError = err;
		blog(LOG_WARNING, "[HFR-SLIDES] 构建渲染项失败：%ls", err.c_str());
		return;
	}
	ctx->loaded = true;
	ctx->needUpload = true;
	blog(LOG_INFO, "[HFR-SLIDES] 已载入 %s：%zu 页，当前第 %d 页，渲染项 %zu", QFileInfo(ctx->file).fileName().toUtf8().constData(),
	     ctx->deck.slides.size(), ctx->page + 1, ctx->slide.items.size());
}

static void hfr_slides_update(void *data, obs_data_t *settings)
{
	HfrSlidesCtx *ctx = static_cast<HfrSlidesCtx *>(data);
	if (!ctx) {
		return;
	}
	const char *file = obs_data_get_string(settings, "file");
	const QString newFile = file ? QString::fromUtf8(file) : QString();
	const int newPage = (int)obs_data_get_int(settings, "page");
	const int newW = (int)obs_data_get_int(settings, "width");
	const int newH = (int)obs_data_get_int(settings, "height");

	const bool fileChanged = (newFile != ctx->file);
	ctx->file = newFile;
	ctx->width = (newW > 0) ? newW : 1920;
	ctx->height = (newH > 0) ? newH : 1080;
	ctx->page = newPage;

	/* 页变化且已载入：只重光栅化，不重新解析 */
	if (!fileChanged && ctx->loaded && !ctx->deck.slides.empty()) {
		std::wstring err;
		if (ctx->page < 0 || ctx->page >= (int)ctx->deck.slides.size()) {
			ctx->page = 0;
		}
		if (hfr::BuildSlide(ctx->deck, ctx->page, ctx->width, ctx->height, ctx->slide, err)) {
			ctx->needUpload = true;
			hfr_slides_reset_timeline(ctx);
			start_transition(ctx);
		} else {
			ctx->lastError = err;
		}
		return;
	}
	hfr_slides_rebuild(ctx);
}

static void hfr_slides_destroy(void *data)
{
	HfrSlidesCtx *ctx = static_cast<HfrSlidesCtx *>(data);
	if (!ctx) {
		return;
	}
	if (!ctx->gpuOld.empty()) {
		obs_enter_graphics();
		destroy_items(ctx->gpuOld);
		obs_leave_graphics();
	}
	hfr_slides_destroy_gpu(ctx);
	for (const HfrSlidesGpuItem &it : ctx->gpu) {
		if (!it.tmpPath.empty()) {
			QFile::remove(QString::fromStdString(it.tmpPath));
		}
	}
	delete ctx;
}

static uint32_t hfr_slides_get_width(void *data)
{
	HfrSlidesCtx *ctx = static_cast<HfrSlidesCtx *>(data);
	return ctx ? (uint32_t)ctx->width : 0;
}

static uint32_t hfr_slides_get_height(void *data)
{
	HfrSlidesCtx *ctx = static_cast<HfrSlidesCtx *>(data);
	return ctx ? (uint32_t)ctx->height : 0;
}

/* 销毁指定纹理集合（需图形上下文） */
static void destroy_items(std::vector<HfrSlidesGpuItem> &items)
{
	for (HfrSlidesGpuItem &it : items) {
		if (it.tex) {
			gs_texture_destroy(it.tex);
			it.tex = nullptr;
		}
	}
	items.clear();
}

/* 启动页间切换（UI 线程调用）：保留上一页纹理 */
static void start_transition(HfrSlidesCtx *ctx)
{
	if (!ctx || ctx->deck.slides.empty() || ctx->page >= (int)ctx->deck.slides.size()) {
		ctx->transActive = false;
		return;
	}
	const hfr::Slide &sl = ctx->deck.slides[(size_t)ctx->page];
	ctx->transActive = true;
	ctx->transStartNs = os_gettime_ns();
	ctx->transDurMs = (sl.transMs > 1.0) ? sl.transMs : 500.0;
	ctx->transDirX = 0;
	ctx->transDirY = 0;
	const std::wstring &t = sl.transType;
	const std::wstring &d = sl.transDir;
	if (t == L"fade" || t == L"dissolve") {
		ctx->transKind = 1;
	} else if (t == L"push" || t == L"cover") {
		ctx->transKind = 2;
	} else if (t == L"wipe") {
		ctx->transKind = 3;
	} else {
		ctx->transKind = 1; /* 未知类型退化为淡入 */
	}
	if (d == L"l") {
		ctx->transDirX = 1; /* 新页从右边推入 */
	} else if (d == L"r") {
		ctx->transDirX = -1;
	} else if (d == L"u") {
		ctx->transDirY = 1;
	} else if (d == L"d") {
		ctx->transDirY = -1;
	}
}

/* 图形线程：上传纹理 */
static void hfr_slides_upload(HfrSlidesCtx *ctx)
{
	if (ctx->transActive && !ctx->gpu.empty()) {
		/* 把当前页纹理移交为"上一页"，等切换结束后释放（需图形上下文） */
		obs_enter_graphics();
		destroy_items(ctx->gpuOld);
		obs_leave_graphics();
		ctx->gpuOld = std::move(ctx->gpu);
		ctx->gpu.clear();
	} else if (!ctx->transActive && !ctx->gpuOld.empty()) {
		obs_enter_graphics();
		destroy_items(ctx->gpuOld);
		obs_leave_graphics();
	}
	hfr_slides_destroy_gpu(ctx);
	const QString cache = HfrPptCacheDir();
	/* Rect 用 1x1 纯色纹理；Text 用 BGRA；Picture 写临时文件后用 gs 加载 */
	obs_enter_graphics();
	for (const hfr::Item &it : ctx->slide.items) {
		HfrSlidesGpuItem g;
		g.kind = it.kind;
		g.x = it.x;
		g.y = it.y;
		g.w = it.w;
		g.h = it.h;
		g.rot60k = it.rot60k;
		g.shapeIndex = it.shapeIndex;
		if (it.kind == hfr::ItemKind::Rect) {
			uint32_t px = it.color;
			g.tex = gs_texture_create(1, 1, GS_BGRA, 1, (const uint8_t **)&px, 0);
		} else if (it.kind == hfr::ItemKind::Text || it.kind == hfr::ItemKind::Gradient) {
			if (!it.pix.empty() && it.texW > 0 && it.texH > 0) {
				const uint8_t *data = it.pix.data();
				g.tex = gs_texture_create((uint32_t)it.texW, (uint32_t)it.texH, GS_BGRA, 1, &data, 0);
			}
		} else if (it.kind == hfr::ItemKind::Picture) {
			if (!it.pix.empty() && it.texW > 0 && it.texH > 0) {
				/* 已解码为 BGRA：直接建纹理 */
				const uint8_t *data = it.pix.data();
				g.tex = gs_texture_create((uint32_t)it.texW, (uint32_t)it.texH, GS_BGRA, 1, &data, 0);
			} else if (!it.pix.empty()) {
				QString ext = it.ext.empty() ? QStringLiteral("png") : QString::fromStdWString(it.ext);
				const QString tmp = cache + QStringLiteral("/img_") + QString::number(ctx->gpu.size()) +
						    QStringLiteral(".") + ext;
				QFile f(tmp);
				if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
					f.write((const char *)it.pix.data(), (qint64)it.pix.size());
					f.close();
					g.tmpPath = tmp.toStdString();
					g.tex = gs_texture_create_from_file(tmp.toUtf8().constData());
					QFile::remove(tmp);
				}
			}
		}
		if (g.tex) {
			ctx->gpu.push_back(g);
		}
	}
	obs_leave_graphics();
	ctx->needUpload = false;
}

static void hfr_slides_render(void *data, gs_effect_t *)
{
	HfrSlidesCtx *ctx = static_cast<HfrSlidesCtx *>(data);
	if (!ctx) {
		return;
	}
	if (ctx->needUpload) {
		hfr_slides_upload(ctx);
	}
	if (ctx->needDestroyOld) {
		obs_enter_graphics();
		destroy_items(ctx->gpuOld);
		obs_leave_graphics();
		ctx->needDestroyOld = false;
	}
	if (ctx->gpu.empty() && ctx->gpuOld.empty()) {
		return;
	}
	/* 切换进度 */
	float tp = 1.0f;
	if (ctx->transActive) {
		const double el = (double)(os_gettime_ns() - ctx->transStartNs) / 1000000.0;
		tp = (float)(el / (ctx->transDurMs > 1.0 ? ctx->transDurMs : 500.0));
		if (tp >= 1.0f) {
			tp = 1.0f;
			ctx->transActive = false;
			ctx->needDestroyOld = true;
		}
		if (tp < 0.0f) {
			tp = 0.0f;
		}
	}
	const float te = 1.0f - (1.0f - tp) * (1.0f - tp); /* EaseOut */
	gs_effect_t *eff = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_technique_t *tech = gs_effect_get_technique(eff, "Draw");
	gs_eparam_t *img = gs_effect_get_param_by_name(eff, "image");
	gs_eparam_t *mul = gs_effect_get_param_by_name(eff, "multiplier");
	gs_technique_begin(tech);

	/* 切换中：先画上一页（推入时同步位移） */
	if (ctx->transActive && !ctx->gpuOld.empty()) {
		for (const HfrSlidesGpuItem &it : ctx->gpuOld) {
			if (!it.tex) {
				continue;
			}
			gs_technique_begin_pass(tech, 0);
			gs_effect_set_texture(img, it.tex);
			if (mul) {
				gs_effect_set_float(mul, 1.0f);
			}
			gs_matrix_push();
			float ox = 0.0f, oy = 0.0f;
			if (ctx->transKind == 2) {
				ox = (float)ctx->transDirX * (float)ctx->width * te;
				oy = (float)ctx->transDirY * (float)ctx->height * te;
			}
			gs_matrix_translate3f((float)it.x + it.dx + ox, (float)it.y + it.dy + oy, 0.0f);
			gs_draw_sprite(it.tex, 0, (uint32_t)it.w, (uint32_t)it.h);
			gs_matrix_pop();
			gs_technique_end_pass(tech);
		}
	}

	for (const HfrSlidesGpuItem &it : ctx->gpu) {
		if (!it.tex || it.alpha <= 0.001f) {
			continue;
		}
		gs_technique_begin_pass(tech, 0);
		gs_effect_set_texture(img, it.tex);
		float alpha = it.alpha;
		float tx = it.dx, ty = it.dy;
		if (ctx->transActive) {
			if (ctx->transKind == 1 || ctx->transKind == 3) {
				alpha = it.alpha * te; /* 淡入 / 擦除近似 */
			} else if (ctx->transKind == 2) {
				tx += (float)(-ctx->transDirX) * (float)ctx->width * (1.0f - te);
				ty += (float)(-ctx->transDirY) * (float)ctx->height * (1.0f - te);
			}
		}
		if (mul) {
			gs_effect_set_float(mul, alpha);
		}
		gs_matrix_push();
		gs_matrix_translate3f((float)it.x + tx, (float)it.y + ty, 0.0f);
		if (it.scale != 1.0f || it.rot60k) {
			const float cx = (float)it.w / 2.0f, cy = (float)it.h / 2.0f;
			gs_matrix_translate3f(cx, cy, 0.0f);
			if (it.rot60k) {
				const float deg = (float)it.rot60k / 60000.0f;
				gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, deg * 3.14159265f / 180.0f);
			}
			if (it.scale != 1.0f) {
				gs_matrix_scale3f(it.scale, it.scale, 1.0f);
			}
			gs_matrix_translate3f(-cx, -cy, 0.0f);
		}
		gs_draw_sprite(it.tex, 0, (uint32_t)it.w, (uint32_t)it.h);
		gs_matrix_pop();
		gs_technique_end_pass(tech);
		if (mul) {
			gs_effect_set_float(mul, 1.0f);
		}
	}
	gs_technique_end(tech);
}

/* ---------------- 属性面板 ---------------- */
static obs_properties_t *hfr_slides_properties(void *data)
{
	HfrSlidesCtx *ctx = static_cast<HfrSlidesCtx *>(data);
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_text(props, "hfr_slides_info",
				"由 HuafeiRongOBS 自研 PPT 引擎渲染（OOXML 解析 + GPU 绘制），支持实时动画与多画布独立进度。",
				OBS_TEXT_INFO);

	obs_properties_add_path(props, "file", "演示文稿", OBS_PATH_FILE, "演示文稿 (*.pptx *.ppt *.odp)", nullptr);

	obs_property_t *pageProp = obs_properties_add_int(props, "page", "页码（从 0 开始）", 0, 9999, 1);
	(void)pageProp;

	obs_properties_add_int(props, "width", "渲染宽度", 160, 7680, 2);
	obs_properties_add_int(props, "height", "渲染高度", 90, 4320, 2);

	obs_properties_add_button2(props, "hfr_slides_prev", "◀ 上一页", [](obs_properties_t *, obs_property_t *, void *data) {
		HfrSlidesCtx *c = static_cast<HfrSlidesCtx *>(data);
		if (c && c->page > 0) {
			c->page--;
			std::wstring err;
			if (hfr::BuildSlide(c->deck, c->page, c->width, c->height, c->slide, err)) {
				c->needUpload = true;
			}
		}
		return true;
	}, data);
	obs_properties_add_button2(props, "hfr_slides_next", "下一页 ▶", [](obs_properties_t *, obs_property_t *, void *data) {
		HfrSlidesCtx *c = static_cast<HfrSlidesCtx *>(data);
		if (c && c->page + 1 < (int)c->deck.slides.size()) {
			c->page++;
			std::wstring err;
			if (hfr::BuildSlide(c->deck, c->page, c->width, c->height, c->slide, err)) {
				c->needUpload = true;
			}
		}
		return true;
	}, data);
	obs_properties_add_button2(props, "hfr_slides_anim_next", "▶ 下一步动画（点击/翻页）",
				   [](obs_properties_t *, obs_property_t *, void *data) {
		HfrSlidesCtx *c = static_cast<HfrSlidesCtx *>(data);
		if (c) {
			const int total = step_count(c);
			if (c->animStep < total) {
				c->animStep++;
				start_step_anim(c);
			}
		}
		return true;
	}, data);
	obs_properties_add_button2(props, "hfr_slides_anim_prev", "◀ 上一步动画",
				   [](obs_properties_t *, obs_property_t *, void *data) {
		HfrSlidesCtx *c = static_cast<HfrSlidesCtx *>(data);
		if (c && c->animStep > 0) {
			c->animStep--;
			c->animating = false;
		}
		return true;
	}, data);
	obs_properties_add_button2(props, "hfr_slides_anim_reset", "⟲ 重置动画",
				   [](obs_properties_t *, obs_property_t *, void *data) {
		HfrSlidesCtx *c = static_cast<HfrSlidesCtx *>(data);
		if (c) {
			c->animStep = 0;
			c->animating = false;
		}
		return true;
	}, data);
	obs_properties_add_button2(props, "hfr_slides_reload", "重新载入文件", [](obs_properties_t *, obs_property_t *, void *data) {
		HfrSlidesCtx *c = static_cast<HfrSlidesCtx *>(data);
		if (c) {
			hfr_slides_rebuild(c);
		}
		return true;
	}, data);

	if (ctx && !ctx->lastError.empty()) {
		const QString msg = QStringLiteral("状态：") + QString::fromWCharArray(ctx->lastError.c_str());
		obs_properties_add_text(props, "hfr_slides_status", msg.toUtf8().constData(), OBS_TEXT_INFO);
	}
	return props;
}

static void *hfr_slides_create(obs_data_t *settings, obs_source_t *source)
{
	HfrSlidesCtx *ctx = new HfrSlidesCtx();
	ctx->width = 1920;
	ctx->height = 1080;
	ctx->file = QString::fromUtf8(obs_data_get_string(settings, "file"));
	hfr_slides_update(ctx, settings);
	return ctx;
}

/* 缓动（近似 PowerPoint 的平滑进入） */
static float EaseOut(float p)
{
	if (p <= 0.0f) {
		return 0.0f;
	}
	if (p >= 1.0f) {
		return 1.0f;
	}
	return 1.0f - (1.0f - p) * (1.0f - p);
}

/* 取某形状在某步的初始/最终表现，并按进度插值（图形线程：只算数学） */
static void EvalItemAnim(HfrSlidesCtx *ctx, HfrSlidesGpuItem &it, uint64_t nowNs)
{
	it.alpha = 1.0f;
	it.dx = 0.0f;
	it.dy = 0.0f;
	it.scale = 1.0f;
	if (!ctx->loaded || ctx->slide.items.empty()) {
		return;
	}
	const std::vector<std::vector<hfr::Anim>> &steps = ctx->deck.slides[(size_t)ctx->page].animSteps;
	if (steps.empty()) {
		return;
	}
	/* 只看本项所属形状相关的动画 */
	for (size_t si = 0; si < steps.size(); si++) {
		for (const hfr::Anim &a : steps[si]) {
			if (a.shapeIndex != it.shapeIndex) {
				continue;
			}
			const bool done = ((int)si < ctx->animStep);
			const bool current = ((int)si == ctx->animStep) && ctx->animating;
			const bool isExit = (a.presetClass == L"exit");
			const bool isEmph = (a.presetClass == L"emph");
			const std::wstring &f = a.filter;

			float p = 0.0f;
			if (done) {
				p = 1.0f;
			} else if (current) {
				const double elapsedMs = (double)(nowNs - ctx->animStartNs) / 1000000.0 - a.delayMs;
				const double dur = (a.durMs > 1.0) ? a.durMs : 500.0;
				p = (float)(elapsedMs / dur);
				if (p < 0.0f) {
					p = 0.0f;
				}
				if (p > 1.0f) {
					p = 1.0f;
				}
			} else {
				/* 尚未轮到：进入类效果保持"未出现"，退出类保持"在场" */
				p = isExit ? 0.0f : 0.0f;
			}
			const float e = EaseOut(p);

			if (isEmph) {
				it.scale = 1.0f + 0.25f * (float)sin(p * 3.14159265);
				continue;
			}
			if (isExit) {
				it.alpha = 1.0f - e;
				continue;
			}
			/* 进入类 */
			if (f.find(L"fade") != std::wstring::npos || f.empty() || a.presetId == 10) {
				it.alpha = e;
			} else if (f.find(L"wipe") != std::wstring::npos) {
				/* 近似：横向展开 + 淡入 */
				it.scale = 1.0f;
				it.dx = (float)((1.0 - e) * -40.0);
				it.alpha = e;
			} else if (f.find(L"slide") != std::wstring::npos || f.find(L"fly") != std::wstring::npos ||
				   f.find(L"ppt_") != std::wstring::npos || a.presetId == 2) {
				it.dx = (float)((1.0 - e) * -300.0);
				it.alpha = e;
			} else if (f.find(L"zoom") != std::wstring::npos || a.presetId == 23) {
				it.scale = 0.4f + 0.6f * e;
				it.alpha = e;
			} else {
				it.alpha = e;
			}
		}
	}
}

static void hfr_slides_tick(void *data, float)
{
	HfrSlidesCtx *ctx = static_cast<HfrSlidesCtx *>(data);
	if (!ctx) {
		return;
	}
	const uint64_t now = os_gettime_ns();
	if (ctx->animating) {
		const double elapsedMs = (double)(now - ctx->animStartNs) / 1000000.0;
		if (elapsedMs >= ctx->animDurationMs) {
			ctx->animating = false;
		}
	}
	for (HfrSlidesGpuItem &it : ctx->gpu) {
		EvalItemAnim(ctx, it, now);
	}
}

bool HfrRegisterSlidesSource()
{
	static bool registered = false;
	if (registered) {
		return true;
	}
	struct obs_source_info info = {};
	info.id = "hfr_slides_source";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
	info.get_name = hfr_slides_get_name;
	info.create = hfr_slides_create;
	info.destroy = hfr_slides_destroy;
	info.update = hfr_slides_update;
	info.video_render = hfr_slides_render;
	info.video_tick = hfr_slides_tick;
	info.get_width = hfr_slides_get_width;
	info.get_height = hfr_slides_get_height;
	info.get_properties = hfr_slides_properties;
	obs_register_source(&info);
	registered = true;
	blog(LOG_INFO, "[HFR-SLIDES] 已注册来源 hfr_slides_source（自研引擎）");
	return true;
}

/* ---------------- 外部控制（UI 线程） ---------------- */
static HfrSlidesCtx *ctx_of(obs_source_t *src)
{
	if (!src) {
		return nullptr;
	}
	const char *id = obs_source_get_id(src);
	if (!id || strcmp(id, "hfr_slides_source") != 0) {
		return nullptr;
	}
	return static_cast<HfrSlidesCtx *>(obs_obj_get_data(src));
}

void HfrSlidesSetFile(obs_source_t *src, const QString &path)
{
	HfrSlidesCtx *ctx = ctx_of(src);
	if (!ctx) {
		return;
	}
	ctx->file = path;
	ctx->page = 0;
	hfr_slides_rebuild(ctx);
}

void HfrSlidesSetPage(obs_source_t *src, int page)
{
	HfrSlidesCtx *ctx = ctx_of(src);
	if (!ctx) {
		return;
	}
	ctx->page = page;
	std::wstring err;
	if (hfr::BuildSlide(ctx->deck, ctx->page, ctx->width, ctx->height, ctx->slide, err)) {
		ctx->needUpload = true;
		hfr_slides_reset_timeline(ctx);
		start_transition(ctx);
	} else {
		ctx->lastError = err;
	}
}

int HfrSlidesPage(obs_source_t *src)
{
	HfrSlidesCtx *ctx = ctx_of(src);
	return ctx ? ctx->page : -1;
}

int HfrSlidesPageCount(obs_source_t *src)
{
	HfrSlidesCtx *ctx = ctx_of(src);
	return ctx ? (int)ctx->deck.slides.size() : 0;
}

static bool hfr_find_slides_cb(void *param, obs_source_t *src)
{
	if (!src) {
		return true;
	}
	const char *id = obs_source_get_id(src);
	if (id && strcmp(id, "hfr_slides_source") == 0) {
		*static_cast<obs_source_t **>(param) = src;
		return false;
	}
	return true;
}

/* ---------------- 动画控制 ---------------- */
static int step_count(HfrSlidesCtx *ctx)
{
	if (!ctx || ctx->deck.slides.empty() || ctx->page >= (int)ctx->deck.slides.size()) {
		return 0;
	}
	return (int)ctx->deck.slides[(size_t)ctx->page].animSteps.size();
}

static void start_step_anim(HfrSlidesCtx *ctx)
{
	if (!ctx) {
		return;
	}
	/* 本步内最长时长决定动画何时结束 */
	double dur = 0.0;
	if (ctx->page < (int)ctx->deck.slides.size()) {
		const auto &steps = ctx->deck.slides[(size_t)ctx->page].animSteps;
		if (ctx->animStep >= 0 && ctx->animStep < (int)steps.size()) {
			for (const hfr::Anim &a : steps[(size_t)ctx->animStep]) {
				dur = std::max(dur, a.delayMs + a.durMs);
			}
		}
	}
	ctx->animDurationMs = (dur > 0.0) ? dur : 0.0;
	ctx->animStartNs = os_gettime_ns();
	ctx->animating = (ctx->animDurationMs > 0.0);
}

std::wstring HfrSlidesNotes(obs_source_t *src)
{
	HfrSlidesCtx *ctx = ctx_of(src);
	if (!ctx || ctx->deck.slides.empty() || ctx->page >= (int)ctx->deck.slides.size()) {
		return std::wstring();
	}
	return ctx->deck.slides[(size_t)ctx->page].notes;
}

bool HfrSlidesRenderPreview(obs_source_t *src, int page, int w, int h, std::vector<uint8_t> &bgraOut)
{
	HfrSlidesCtx *ctx = ctx_of(src);
	if (!ctx || ctx->deck.slides.empty() || w <= 0 || h <= 0) {
		return false;
	}
	hfr::RenderSlide rs;
	std::wstring err;
	if (!hfr::BuildSlide(ctx->deck, page, w, h, rs, err)) {
		return false;
	}
	hfr::CompositeToBgra(rs, bgraOut);
	return true;
}

int HfrSlidesAnimStep(obs_source_t *src)
{
	HfrSlidesCtx *ctx = ctx_of(src);
	return ctx ? ctx->animStep : -1;
}

int HfrSlidesAnimStepCount(obs_source_t *src)
{
	return step_count(ctx_of(src));
}

bool HfrSlidesNextAnim(obs_source_t *src)
{
	HfrSlidesCtx *ctx = ctx_of(src);
	if (!ctx) {
		return false;
	}
	const int total = step_count(ctx);
	if (ctx->animStep >= total) {
		return false;
	}
	ctx->animStep++;
	start_step_anim(ctx);
	blog(LOG_INFO, "[HFR-SLIDES] 动画步进 %d/%d", ctx->animStep, total);
	return true;
}

bool HfrSlidesPrevAnim(obs_source_t *src)
{
	HfrSlidesCtx *ctx = ctx_of(src);
	if (!ctx) {
		return false;
	}
	if (ctx->animStep <= 0) {
		return false;
	}
	ctx->animStep--;
	ctx->animating = false;
	return true;
}

void HfrSlidesResetAnim(obs_source_t *src)
{
	HfrSlidesCtx *ctx = ctx_of(src);
	if (!ctx) {
		return;
	}
	ctx->animStep = 0;
	ctx->animating = false;
}

obs_source_t *HfrFindFirstSlidesSource()
{
	obs_source_t *found = nullptr;
	obs_enum_sources(hfr_find_slides_cb, &found);
	return found;
}
