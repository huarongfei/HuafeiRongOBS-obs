#include "HFRPptSource.hpp"

#include "HFRPpt.hpp"

#include <obs-module.h>

#include <cstring>
#include <vector>

#ifdef HFR_ENABLE_PPT
struct HfrPptCtx {
	HFRPptDocument doc;
	gs_texture_t *tex = nullptr;
	std::vector<uint8_t> buf;
	bool needCpuRender = false;
	bool needUpload = false;
	uint32_t w = 1280;
	uint32_t h = 720;
	std::string file;
	int page = 0;
	std::string lastError;
};

static const char *hfr_ppt_get_name(void *)
{
	return "PPT 演示文稿（LibreOffice）";
}

static void *hfr_ppt_create(obs_data_t *settings, obs_source_t *)
{
	HfrPptCtx *ctx = new HfrPptCtx();
	ctx->w = (uint32_t)obs_data_get_int(settings, "width");
	ctx->h = (uint32_t)obs_data_get_int(settings, "height");
	if (ctx->w == 0) {
		ctx->w = 1280;
	}
	if (ctx->h == 0) {
		ctx->h = 720;
	}
	const char *f = obs_data_get_string(settings, "file");
	if (f && *f) {
		ctx->file = f;
	}
	ctx->page = (int)obs_data_get_int(settings, "page");
	ctx->needCpuRender = true;
	return ctx;
}

static void hfr_ppt_destroy(void *data)
{
	HfrPptCtx *ctx = static_cast<HfrPptCtx *>(data);
	if (!ctx) {
		return;
	}
	if (ctx->tex) {
		obs_enter_graphics();
		gs_texture_destroy(ctx->tex);
		obs_leave_graphics();
		ctx->tex = nullptr;
	}
	ctx->doc.Close();
	delete ctx;
}

static uint32_t hfr_ppt_get_width(void *data)
{
	HfrPptCtx *ctx = static_cast<HfrPptCtx *>(data);
	return ctx ? ctx->w : 0;
}

static uint32_t hfr_ppt_get_height(void *data)
{
	HfrPptCtx *ctx = static_cast<HfrPptCtx *>(data);
	return ctx ? ctx->h : 0;
}

static void hfr_ppt_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "file", "");
	obs_data_set_default_int(settings, "width", 1280);
	obs_data_set_default_int(settings, "height", 720);
	obs_data_set_default_int(settings, "page", 0);
}

static void hfr_ppt_render_now(HfrPptCtx *ctx); /* 前置声明（定义见下） */

static void hfr_ppt_load(HfrPptCtx *ctx)
{
	if (ctx->file.empty()) {
		return;
	}
	QString err;
	if (!ctx->doc.IsOpen()) {
		if (!ctx->doc.Open(QString::fromStdString(ctx->file), &err)) {
			ctx->lastError = err.toStdString();
			blog(LOG_WARNING, "[HFR-PPT-SOURCE] 打开失败：%s", ctx->lastError.c_str());
			return;
		}
	}
	const int parts = ctx->doc.PartCount();
	if (parts > 0) {
		int p = ctx->page;
		if (p < 0) {
			p = 0;
		}
		if (p >= parts) {
			p = parts - 1;
		}
		ctx->doc.SetPart(p);
	}
	ctx->lastError.clear();
	ctx->needCpuRender = true;
}

static void hfr_ppt_update(void *data, obs_data_t *settings)
{
	HfrPptCtx *ctx = static_cast<HfrPptCtx *>(data);
	if (!ctx) {
		return;
	}
	const char *f = obs_data_get_string(settings, "file");
	const std::string newFile = f ? f : "";
	const int newPage = (int)obs_data_get_int(settings, "page");
	const uint32_t nw = (uint32_t)obs_data_get_int(settings, "width");
	const uint32_t nh = (uint32_t)obs_data_get_int(settings, "height");

	const bool fileChanged = (newFile != ctx->file);
	ctx->file = newFile;
	if (nw) {
		ctx->w = nw;
	}
	if (nh) {
		ctx->h = nh;
	}
	ctx->page = newPage;

	if (fileChanged) {
		ctx->doc.Close();
	}
	hfr_ppt_load(ctx);
	/* 在 UI 线程内完成渲染（不得留到图形线程） */
	hfr_ppt_render_now(ctx);
}

/* UI 线程调用：按当前页渲染并标记需要上传纹理（不触碰图形线程） */
static void hfr_ppt_render_now(HfrPptCtx *ctx)
{
	if (!ctx || !ctx->doc.IsOpen()) {
		return;
	}
	QString err;
	if (ctx->doc.RenderCurrentPart(ctx->w, ctx->h, ctx->buf, &err)) {
		ctx->needUpload = true;
		ctx->needCpuRender = false;
		ctx->lastError.clear();
	} else {
		ctx->lastError = err.toStdString();
	}
}

/* 按钮：上一页 / 下一页 / 重新载入 —— data 为来源的 ctx */
static bool hfr_ppt_btn_prev(obs_properties_t *, obs_property_t *, void *data)
{
	HfrPptCtx *ctx = static_cast<HfrPptCtx *>(data);
	if (ctx && ctx->doc.IsOpen()) {
		ctx->doc.PrevPart();
		ctx->page = ctx->doc.CurrentPart();
		ctx->needCpuRender = true;
	}
	return true;
}

static bool hfr_ppt_btn_next(obs_properties_t *, obs_property_t *, void *data)
{
	HfrPptCtx *ctx = static_cast<HfrPptCtx *>(data);
	if (ctx && ctx->doc.IsOpen()) {
		ctx->doc.NextPart();
		ctx->page = ctx->doc.CurrentPart();
		ctx->needCpuRender = true;
	}
	return true;
}

static bool hfr_ppt_btn_reload(obs_properties_t *, obs_property_t *, void *data)
{
	HfrPptCtx *ctx = static_cast<HfrPptCtx *>(data);
	if (ctx) {
		ctx->doc.Close();
		hfr_ppt_load(ctx);
		ctx->needUpload = true;
	}
	return true;
}

static obs_properties_t *hfr_ppt_properties(void *data)
{
	HfrPptCtx *ctx = static_cast<HfrPptCtx *>(data);
	obs_properties_t *props = obs_properties_create();

	/* —— 来源与授权声明（用户添加本来源、打开属性面板时可见）—— */
	{
		const QString ver = HfrLibreOfficeVersion();
		QString attr = QStringLiteral(
			"本功能基于 LibreOffice 开发：PPT 由 LibreOffice 的 LibreOfficeKit（LOK）在本进程内渲染，"
			"不依赖 PowerPoint。LibreOffice 是 The Document Foundation 的商标，"
			"按 MPL-2.0 / LGPLv3+ 授权发布。");
		if (!ver.isEmpty()) {
			attr += QStringLiteral(" 检测到的 LibreOffice 版本：%1。").arg(ver);
		} else {
			attr += QStringLiteral(" 当前尚未加载 LibreOffice（可用环境变量 HFR_LO_PATH 指定其安装目录）。");
		}
		obs_properties_add_text(props, "hfr_ppt_attribution", attr.toUtf8().constData(), OBS_TEXT_INFO);
	}

	obs_properties_add_path(props, "file", "PPT 文件", OBS_PATH_FILE,
				"演示文稿 (*.pptx *.ppt *.odp)", nullptr);
	obs_properties_add_int(props, "width", "渲染宽度", 160, 7680, 1);
	obs_properties_add_int(props, "height", "渲染高度", 90, 4320, 1);
	obs_properties_add_int(props, "page", "页码（从 0 开始）", 0, 9999, 1);

	obs_properties_add_button2(props, "btn_prev", "上一页", hfr_ppt_btn_prev, data);
	obs_properties_add_button2(props, "btn_next", "下一页", hfr_ppt_btn_next, data);
	obs_properties_add_button2(props, "btn_reload", "重新载入文件", hfr_ppt_btn_reload, data);

	if (ctx) {
		QString info;
		if (!ctx->lastError.empty()) {
			info = QStringLiteral("错误：%1").arg(QString::fromStdString(ctx->lastError));
		} else if (ctx->doc.IsOpen()) {
			info = QStringLiteral("已载入：共 %1 页，当前第 %2 页")
				       .arg(ctx->doc.PartCount())
				       .arg(ctx->doc.CurrentPart() + 1);
		} else {
			info = QStringLiteral("尚未载入文件。若提示找不到 LibreOffice，请设置环境变量 HFR_LO_PATH 指向其安装目录。");
		}
		obs_properties_add_text(props, "info", info.toUtf8().constData(), OBS_TEXT_INFO);
	}
	return props;
}

/* 注意：video_tick 运行在 OBS 图形线程上，绝对不能调用 LibreOffice（会崩，
 * 见历史崩溃栈 tick_sources→hfr_ppt_tick→HFRPptDocument::Open→mergedlo.dll）。
 * 所有 LOK 调用统一走 UI 线程：属性变更(update)、按钮、讲者视图翻页。 */
static void hfr_ppt_tick(void *data, float)
{
	HfrPptCtx *ctx = static_cast<HfrPptCtx *>(data);
	if (!ctx) {
		return;
	}
	/* 若 UI 线程尚未渲染出内容，这里只做标记，绝不触发 LOK */
	if (!ctx->doc.IsOpen()) {
		ctx->needCpuRender = false;
	}
}

static void hfr_ppt_render(void *data, gs_effect_t *)
{
	HfrPptCtx *ctx = static_cast<HfrPptCtx *>(data);
	if (!ctx) {
		return;
	}
	if (ctx->needUpload && !ctx->buf.empty()) {
		const uint32_t bytesPerRow = ctx->w * 4;
		if (!ctx->tex) {
			const uint8_t *ptr = ctx->buf.data();
			ctx->tex = gs_texture_create(ctx->w, ctx->h, GS_BGRA, 1, &ptr, GS_DYNAMIC);
		} else {
			gs_texture_set_image(ctx->tex, ctx->buf.data(), bytesPerRow, false);
		}
		ctx->needUpload = false;
	}
	if (ctx->tex) {
		obs_source_draw(ctx->tex, 0, 0, 0, 0, false);
	}
}

static struct obs_source_info hfr_ppt_source_info = {};

HFRPptDocument *HfrPptSourceGetDoc(obs_source_t *source)
{
	if (!source) {
		return nullptr;
	}
	const char *id = obs_source_get_id(source);
	if (!id || strcmp(id, "hfr_ppt_source") != 0) {
		return nullptr;
	}
	HfrPptCtx *ctx = static_cast<HfrPptCtx *>(obs_obj_get_data(source));
	return ctx ? &ctx->doc : nullptr;
}

void HfrPptSourceRenderNow(obs_source_t *source)
{
	if (!source) {
		return;
	}
	const char *id = obs_source_get_id(source);
	if (!id || strcmp(id, "hfr_ppt_source") != 0) {
		return;
	}
	HfrPptCtx *ctx = static_cast<HfrPptCtx *>(obs_obj_get_data(source));
	hfr_ppt_render_now(ctx);
}

void HfrPptSourceMarkDirty(obs_source_t *source)
{
	if (!source) {
		return;
	}
	const char *id = obs_source_get_id(source);
	if (!id || strcmp(id, "hfr_ppt_source") != 0) {
		return;
	}
	HfrPptCtx *ctx = static_cast<HfrPptCtx *>(obs_obj_get_data(source));
	if (ctx) {
		ctx->needCpuRender = true;
		ctx->needUpload = true;
	}
}

static bool hfr_find_ppt_cb(void *param, obs_source_t *src)
{
	if (!param || !src) {
		return true;
	}
	const char *id = obs_source_get_id(src);
	if (id && strcmp(id, "hfr_ppt_source") == 0) {
		*static_cast<obs_source_t **>(param) = src;
		return false;
	}
	return true;
}

obs_source_t *HfrFindFirstPptSource()
{
	obs_source_t *found = nullptr;
	obs_enum_sources(hfr_find_ppt_cb, &found);
	return found;
}

bool HfrRegisterPptSource()
{
	static bool registered = false;
	if (registered) {
		return true;
	}
	hfr_ppt_source_info.id = "hfr_ppt_source";
	hfr_ppt_source_info.type = OBS_SOURCE_TYPE_INPUT;
	hfr_ppt_source_info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
	hfr_ppt_source_info.get_name = hfr_ppt_get_name;
	hfr_ppt_source_info.create = hfr_ppt_create;
	hfr_ppt_source_info.destroy = hfr_ppt_destroy;
	hfr_ppt_source_info.get_width = hfr_ppt_get_width;
	hfr_ppt_source_info.get_height = hfr_ppt_get_height;
	hfr_ppt_source_info.get_defaults = hfr_ppt_defaults;
	hfr_ppt_source_info.get_properties = hfr_ppt_properties;
	hfr_ppt_source_info.update = hfr_ppt_update;
	hfr_ppt_source_info.video_tick = hfr_ppt_tick;
	hfr_ppt_source_info.video_render = hfr_ppt_render;
	hfr_ppt_source_info.version = 1;

	obs_register_source(&hfr_ppt_source_info);
	registered = true;
	blog(LOG_INFO, "[HFR-PPT] 来源类型已注册：hfr_ppt_source");
	return true;
}

#else /* !HFR_ENABLE_PPT */

HFRPptDocument *HfrPptSourceGetDoc(obs_source_t *)
{
	return nullptr;
}
void HfrPptSourceMarkDirty(obs_source_t *) {}
obs_source_t *HfrFindFirstPptSource()
{
	return nullptr;
}

bool HfrRegisterPptSource()
{
	/* 未启用时不注册（避免出现无法工作的来源项） */
	return false;
}

#endif