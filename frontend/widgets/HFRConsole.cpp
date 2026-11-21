#include "HFRConsole.hpp"

#include <QCheckBox>

#include <QAction>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScreen>
#include <QSpinBox>
#include <QTabWidget>
#include <QThread>
#include <QVBoxLayout>
#include <QComboBox>
#include <QDir>
#include <QSysInfo>
#include <QTextStream>
#include <QFileDialog>
#include <QDateTime>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QFile>
#include <QSaveFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QVariant>

#include <obs-frontend-api.h>

#include "HFRPresenter.hpp"
#include "HFRPpt.hpp"
#include "HFRAudioRoutingDialog.hpp"
#include "HFRPptPlayer.hpp"
#include "HFRSlidesSource.hpp"
#include "HFRSwitcherPanel.hpp"

#include <vector>

#include <obs-module.h>
#include <util/platform.h>

HFRConsoleDock *HFRConsoleDock::s_instance = nullptr;

/* PPT 自检辅助（定义在文件下部） */
static HFRPptDocument &PptSelfTestDoc();
static void WriteBmp32File(const QString &path, uint32_t w, uint32_t h, const std::vector<uint8_t> &bgra);
static void PptRenderAndReport(QLabel *status);

/* PPT 相关入口声明见 HFRPpt.hpp（HfrPptTestOutputDir / HfrRegisterPptSource） */

static void HfrLog(const char *msg)
{
	blog(LOG_INFO, "[HFR] %s", msg);
}

/* 状态栏分级样式：普通（深灰）/ 警告（橙）/ 错误（红），一眼区分前置条件与失败 */
static void ApplyStatusLevel(QLabel *label, int level)
{
	if (!label) {
		return;
	}
	switch (level) {
	case 1:
		label->setStyleSheet(QStringLiteral("color:#b26a00; font-size:12px; font-weight:bold;"));
		break;
	case 2:
		label->setStyleSheet(QStringLiteral("color:#c62828; font-size:12px; font-weight:bold;"));
		break;
	default:
		label->setStyleSheet(QStringLiteral("color:#222; font-size:12px;"));
		break;
	}
}

void HFRConsoleDock::SetStatus(const QString &text, int level)
{
	if (!status) {
		return;
	}
	status->setText(text);
	ApplyStatusLevel(status, level);
}

/* ---------------------------- HFRCanvasRow ---------------------------- */

HFRCanvasRow::HFRCanvasRow(obs_canvas_t *cv, bool recording, QWidget *parent)
	: QWidget(parent), canvas(cv)
{
	QHBoxLayout *layout = new QHBoxLayout(this);
	layout->setContentsMargins(2, 2, 2, 2);
	layout->setSpacing(6);

	thumb = new OBSQTDisplay(this);
	thumb->setFixedSize(160, 90);
	thumb->setMinimumSize(160, 90);
	layout->addWidget(thumb);

	QVBoxLayout *txt = new QVBoxLayout();
	QLabel *name = new QLabel(this);
	QString title = QString::fromUtf8(obs_canvas_get_name(cv));
	QString prefix;
	if (recording) {
		prefix += QStringLiteral("● REC  ");
	}
	title = prefix + title;
	if (!prefix.isEmpty()) {
		name->setStyleSheet(QStringLiteral("font-weight:bold; color:#e04040;"));
	} else {
		name->setStyleSheet(QStringLiteral("font-weight:bold;"));
	}
	name->setText(title);
	nameLabel = name;
	txt->addWidget(name);

	struct obs_video_info ovi;
	QString sizeText = QStringLiteral("?");
	if (obs_canvas_get_video_info(cv, &ovi)) {
		sizeText = QStringLiteral("%1x%2").arg(ovi.base_width).arg(ovi.base_height);
	}
	QLabel *size = new QLabel(sizeText, this);
	size->setStyleSheet(QStringLiteral("color:#888;"));
	txt->addWidget(size);
	layout->addLayout(txt, 1);

	auto addDraw = [this]() {
		obs_display_add_draw_callback(thumb->GetDisplay(), &HFRCanvasRow::DrawThumb, this);
	};
	connect(thumb, &OBSQTDisplay::DisplayCreated, this, addDraw);
	ready = true;
}

HFRCanvasRow::~HFRCanvasRow()
{
	if (thumb && thumb->GetDisplay()) {
		obs_display_remove_draw_callback(thumb->GetDisplay(), &HFRCanvasRow::DrawThumb, this);
	}
}

void HFRCanvasRow::SetRecording(bool recording)
{
	if (!nameLabel) {
		return;
	}
	const QString base = QString::fromUtf8(obs_canvas_get_name(canvas));
	if (recording) {
		nameLabel->setStyleSheet(QStringLiteral("font-weight:bold; color:#e04040;"));
		nameLabel->setText(QStringLiteral("● REC  ") + base);
	} else {
		nameLabel->setStyleSheet(QStringLiteral("font-weight:bold;"));
		nameLabel->setText(base);
	}
}

void HFRCanvasRow::DrawThumb(void *data, uint32_t, uint32_t)
{
	HFRCanvasRow *row = static_cast<HFRCanvasRow *>(data);
	if (!row || !row->ready || !row->canvas || !row->thumb) {
		return;
	}
	/* 性能：行不可见（滚动出视野/页签隐藏）时不做缩略图渲染 */
	if (!row->isVisible() || row->thumb->visibleRegion().isEmpty()) {
		return;
	}
	/* 优化：缩略图节流到约 10fps（多画布时大幅降低缩略图渲染开销） */
	const qint64 now = (qint64)os_gettime_ns();
	if (row->lastDrawNs != 0 && now - row->lastDrawNs < 100000000LL) {
		return;
	}
	row->lastDrawNs = now;
	obs_render_canvas_texture(row->canvas);
}

/* ------------------------- HFRCanvasProjector ------------------------- */

HFRCanvasProjector::HFRCanvasProjector(obs_canvas_t *canvas_, QScreen *screen, QWidget *parent,
					 QRect tileRect, int tileRot, int blendL_, int blendR_, int blendT_,
					 int blendB_, const HfrWallTile *keystone)
	: OBSQTDisplay(parent), canvas(canvas_)
{
	tile = tileRect;
	hasTile = tileRect.width() > 0 && tileRect.height() > 0;
	tileRot = ((tileRot % 360) + 360) % 360;
	blendL = blendL_;
	blendR = blendR_;
	blendT = blendT_;
	blendB = blendB_;
	if (keystone && keystone->HasCurve()) {
		cMTx = keystone->cMTx;
		cMTy = keystone->cMTy;
		cMRx = keystone->cMRx;
		cMRy = keystone->cMRy;
		cMBx = keystone->cMBx;
		cMBy = keystone->cMBy;
		cMLx = keystone->cMLx;
		cMLy = keystone->cMLy;
		cCx = keystone->cCx;
		cCy = keystone->cCy;
		hasCurve = true;
	}
	if (keystone && keystone->HasColorAdjust()) {
		gammaV = keystone->gammaV;
		liftV = keystone->liftV;
		gainV = keystone->gainV;
		hasColorAdj = true;
	}
	if (keystone && keystone->HasKeystone()) {
		kTLx = keystone->kTLx;
		kTLy = keystone->kTLy;
		kTRx = keystone->kTRx;
		kTRy = keystone->kTRy;
		kBLx = keystone->kBLx;
		kBLy = keystone->kBLy;
		kBRx = keystone->kBRx;
		kBRy = keystone->kBRy;
		hasKeystone = true;
	}
	int idx = QGuiApplication::screens().indexOf(screen);
	QString title;
	if (canvas && screen) {
		title = QStringLiteral("%1 → 屏%2 (%3)")
				.arg(QString::fromUtf8(obs_canvas_get_name(canvas)))
				.arg(idx)
				.arg(screen->name());
	} else {
		title = QStringLiteral("画布投影");
	}
	setWindowTitle(title);
	setAttribute(Qt::WA_DeleteOnClose, true);
	setAttribute(Qt::WA_QuitOnClose, false);

	QAction *escapeAction = new QAction(this);
	escapeAction->setShortcut(Qt::Key_Escape);
	addAction(escapeAction);
	connect(escapeAction, &QAction::triggered, this, &QWidget::close);

	auto addDraw = [this]() {
		obs_display_add_draw_callback(GetDisplay(), &HFRCanvasProjector::Draw, this);
		obs_display_set_background_color(GetDisplay(), 0x000000);
	};
	connect(this, &OBSQTDisplay::DisplayCreated, this, addDraw);

	ready = true;
	if (screen) {
		setGeometry(screen->geometry());
		showFullScreen();
	} else {
		resize(640, 360);
		show();
	}
	raise();
	activateWindow();
}

HFRCanvasProjector::~HFRCanvasProjector()
{
	if (GetDisplay()) {
		obs_display_remove_draw_callback(GetDisplay(), &HFRCanvasProjector::Draw, this);
	}
	if (curveVb) {
		obs_enter_graphics();
		gs_vertexbuffer_destroy(curveVb);
		obs_leave_graphics();
		curveVb = nullptr;
	}
}

/* 安全区矩形叠加（ratio=内缩比例：0.05→90% 标题区，0.10→80% 动作区） */
static void DrawSafeAreaRect(uint32_t cx, uint32_t cy, float ratio, uint32_t color)
{
	if (cx == 0 || cy == 0) {
		return;
	}
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *colorParam = gs_effect_get_param_by_name(solid, "color");
	gs_effect_set_color(colorParam, color);

	const float m = ratio;
	const float x0 = m * (float)cx;
	const float y0 = m * (float)cy;
	const float w = (1.0f - 2.0f * m) * (float)cx;
	const float h = (1.0f - 2.0f * m) * (float)cy;
	const float t = 2.0f;
	if (w <= 0.0f || h <= 0.0f) {
		return;
	}

	auto bar = [&](float bx, float by, float bw, float bh) {
		gs_matrix_push();
		gs_matrix_translate3f(bx, by, 0.0f);
		while (gs_effect_loop(solid, "Solid")) {
			gs_draw_sprite(nullptr, 0, (uint32_t)bw, (uint32_t)bh);
		}
		gs_matrix_pop();
	};

	bar(x0, y0, w, t);
	bar(x0, y0 + h - t, w, t);
	bar(x0, y0, t, h);
	bar(x0 + w - t, y0, t, h);
}

/* 边缘融合渐变：在 dir 方向画 strips 条黑色带，透明度沿 dir 从 0 渐到 1（边缘全黑）。
 * dir: 0=左(靠 x0 最黑) 1=右(靠 x0+w 最黑) 2=上 3=下。坐标系=调用时的 ortho。 */
static void DrawBlendGradient(int dir, float x0, float y0, float w, float h, int strips)
{
	if (w <= 0.0f || h <= 0.0f || strips <= 0) {
		return;
	}
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *cp = gs_effect_get_param_by_name(solid, "color");
	const bool horiz = (dir == 0 || dir == 1);
	for (int i = 0; i < strips; i++) {
		const float t = ((float)i + 0.5f) / (float)strips;
		const float px = horiz ? (x0 + (w / (float)strips) * (float)i) : x0;
		const float py = horiz ? y0 : (y0 + (h / (float)strips) * (float)i);
		const float pw = horiz ? (w / (float)strips) : w;
		const float ph = horiz ? h : (h / (float)strips);
		const float dark = (dir == 0 || dir == 2) ? (1.0f - t) : t;
		const uint32_t alpha = (uint32_t)(dark * 255.0f);
		gs_effect_set_color(cp, alpha << 24);
		gs_matrix_push();
		gs_matrix_translate3f(px, py, 0.0f);
		while (gs_effect_loop(solid, "Solid")) {
			gs_draw_sprite(nullptr, 0, (uint32_t)pw, (uint32_t)ph);
		}
		gs_matrix_pop();
	}
}

/* ---------------- 四角几何校正（keystone）：自定义 shader 双线性四角映射 ---------------- */

static const char *kKeystoneEffectSrc = R"EFF(
uniform float4x4 ViewProj;
uniform float4 quadTL;
uniform float4 quadTR;
uniform float4 quadBL;
uniform float4 quadBR;
uniform float gammaV;
uniform float liftV;
uniform float gainV;
uniform texture2d image;

sampler_state def_sampler {
	Filter   = Linear;
	AddressU = Clamp;
	AddressV = Clamp;
};

struct VertInOut {
	float4 pos : POSITION;
	float2 uv  : TEXCOORD0;
};

VertInOut VSKeystone(VertInOut v)
{
	VertInOut o;
	float2 t = v.uv;
	float2 top = lerp(quadTL.xy, quadTR.xy, t.x);
	float2 bot = lerp(quadBL.xy, quadBR.xy, t.x);
	float2 p = lerp(top, bot, t.y);
	o.pos = mul(float4(p, 0.0, 1.0), ViewProj);
	o.uv = t;
	return o;
}

float4 PSKeystone(VertInOut v) : TARGET
{
	float4 c = image.Sample(def_sampler, v.uv);
	float3 rgb = saturate(c.rgb * gainV + liftV);
	rgb = pow(max(rgb, 0.0001), 1.0 / max(gammaV, 0.01));
	return float4(rgb, c.a);
}

technique Draw
{
	pass
	{
		vertex_shader = VSKeystone(v);
		pixel_shader  = PSKeystone(v);
	}
}
)EFF";

static gs_effect_t *KeystoneEffect()
{
	static gs_effect_t *eff = nullptr;
	static bool tried = false;
	if (!tried) {
		tried = true;
		char *err = nullptr;
		eff = gs_effect_create(kKeystoneEffectSrc, "hfr_keystone.effect", &err);
		if (!eff) {
			blog(LOG_WARNING, "[HFR-KEYSTONE] shader compile failed: %s", err ? err : "?");
		}
		if (err) {
			bfree(err);
		}
	}
	return eff;
}

bool HFRCanvasProjector::DrawKeystoneQuad(gs_texture_t *tex, HFRCanvasProjector *w, uint32_t cx, uint32_t cy)
{
	gs_effect_t *eff = KeystoneEffect();
	if (!eff || !tex) {
		return false;
	}
	const float sx = (float)cx / (float)std::max(1, w->tile.width());
	const float sy = (float)cy / (float)std::max(1, w->tile.height());
	/* 基准四角 = 窗口四角；keystone 偏移按墙像素→窗口像素换算后叠加 */
	struct vec4 tl = {0.0f + (float)w->kTLx * sx, 0.0f + (float)w->kTLy * sy, 0.0f, 0.0f};
	struct vec4 tr = {(float)cx + (float)w->kTRx * sx, 0.0f + (float)w->kTRy * sy, 0.0f, 0.0f};
	struct vec4 bl = {0.0f + (float)w->kBLx * sx, (float)cy + (float)w->kBLy * sy, 0.0f, 0.0f};
	struct vec4 br = {(float)cx + (float)w->kBRx * sx, (float)cy + (float)w->kBRy * sy, 0.0f, 0.0f};
	gs_effect_set_vec4(gs_effect_get_param_by_name(eff, "quadTL"), &tl);
	gs_effect_set_vec4(gs_effect_get_param_by_name(eff, "quadTR"), &tr);
	gs_effect_set_vec4(gs_effect_get_param_by_name(eff, "quadBL"), &bl);
	gs_effect_set_vec4(gs_effect_get_param_by_name(eff, "quadBR"), &br);
	gs_effect_set_float(gs_effect_get_param_by_name(eff, "gammaV"), (float)w->gammaV);
	gs_effect_set_float(gs_effect_get_param_by_name(eff, "liftV"), (float)w->liftV);
	gs_effect_set_float(gs_effect_get_param_by_name(eff, "gainV"), (float)w->gainV);
	gs_effect_set_texture_srgb(gs_effect_get_param_by_name(eff, "image"), tex);
	gs_enable_blending(false);
	gs_matrix_push();
	gs_matrix_identity();
	while (gs_effect_loop(eff, "Draw")) {
		gs_draw_sprite(tex, 0, 0, 0);
	}
	gs_matrix_pop();
	gs_enable_blending(true);
	return true;
}

/* ---------------- 曲面网格（3x3 控制点 → 4 个双线性单元） ---------------- */
static const char *kCurveEffectSrc = R"EFF(
uniform float4x4 ViewProj;
uniform texture2d image;
uniform float gammaV;
uniform float liftV;
uniform float gainV;

sampler_state def_sampler {
	Filter   = Linear;
	AddressU = Clamp;
	AddressV = Clamp;
};

struct VertInOut {
	float4 pos : POSITION;
	float2 uv  : TEXCOORD0;
};

VertInOut VSCurve(VertInOut v)
{
	VertInOut o;
	o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj);
	o.uv = v.uv;
	return o;
}

float4 PSCurve(VertInOut v) : TARGET
{
	float4 c = image.Sample(def_sampler, v.uv);
	float3 rgb = saturate(c.rgb * gainV + liftV);
	rgb = pow(max(rgb, 0.0001), 1.0 / max(gammaV, 0.01));
	return float4(rgb, c.a);
}

technique Draw
{
	pass
	{
		vertex_shader = VSCurve(v);
		pixel_shader  = PSCurve(v);
	}
}
)EFF";

static gs_effect_t *CurveEffect()
{
	static gs_effect_t *eff = nullptr;
	static bool tried = false;
	if (!tried) {
		tried = true;
		char *err = nullptr;
		eff = gs_effect_create(kCurveEffectSrc, "hfr_curve.effect", &err);
		if (!eff) {
			blog(LOG_WARNING, "[HFR-CURVE] shader compile failed: %s", err ? err : "?");
		}
		if (err) {
			bfree(err);
		}
	}
	return eff;
}

/* 边缘融合 + 安全区叠加（窗口坐标；在曲面/四角映射之后调用） */
void HFRCanvasProjector::DrawBlendSafeOverlays(HFRCanvasProjector *w, uint32_t cx, uint32_t cy)
{
	if (!w || !cx || !cy) {
		return;
	}
	if (w->blendL > 0 || w->blendR > 0 || w->blendT > 0 || w->blendB > 0) {
		const float sx = (float)cx / (float)std::max(1, w->tile.width());
		const float sy = (float)cy / (float)std::max(1, w->tile.height());
		gs_viewport_push();
		gs_projection_push();
		gs_set_viewport(0, 0, (int)cx, (int)cy);
		gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
		if (w->blendL > 0) {
			DrawBlendGradient(0, 0.0f, 0.0f, (float)w->blendL * sx, (float)cy, 24);
		}
		if (w->blendR > 0) {
			DrawBlendGradient(1, (float)cx - (float)w->blendR * sx, 0.0f, (float)w->blendR * sx,
					  (float)cy, 24);
		}
		if (w->blendT > 0) {
			DrawBlendGradient(2, 0.0f, 0.0f, (float)cx, (float)w->blendT * sy, 24);
		}
		if (w->blendB > 0) {
			DrawBlendGradient(3, 0.0f, (float)cy - (float)w->blendB * sy, (float)cx,
					  (float)w->blendB * sy, 24);
		}
		gs_projection_pop();
		gs_viewport_pop();
	}
	if (w->HasSafeArea()) {
		gs_viewport_push();
		gs_projection_push();
		gs_set_viewport(0, 0, (int)cx, (int)cy);
		gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
		DrawSafeAreaRect(cx, cy, 0.05f, 0x66FFFFFF);
		DrawSafeAreaRect(cx, cy, 0.10f, 0x66FF4040);
		gs_projection_pop();
		gs_viewport_pop();
	}
}

bool HFRCanvasProjector::DrawCurveMesh(gs_texture_t *tex, HFRCanvasProjector *w, uint32_t cx, uint32_t cy)
{
	gs_effect_t *eff = CurveEffect();
	if (!eff || !tex || !w) {
		return false;
	}
	if (!w->curveVb || w->curveVbW != cx || w->curveVbH != cy) {
		if (w->curveVb) {
			obs_enter_graphics();
			gs_vertexbuffer_destroy(w->curveVb);
			obs_leave_graphics();
			w->curveVb = nullptr;
		}
		const float sx = (float)cx / (float)std::max(1, w->tile.width());
		const float sy = (float)cy / (float)std::max(1, w->tile.height());
		float P[3][3][2];
		P[0][0][0] = 0.0f + (float)w->kTLx * sx;
		P[0][0][1] = 0.0f + (float)w->kTLy * sy;
		P[2][0][0] = (float)cx + (float)w->kTRx * sx;
		P[2][0][1] = 0.0f + (float)w->kTRy * sy;
		P[0][2][0] = 0.0f + (float)w->kBLx * sx;
		P[0][2][1] = (float)cy + (float)w->kBLy * sy;
		P[2][2][0] = (float)cx + (float)w->kBRx * sx;
		P[2][2][1] = (float)cy + (float)w->kBRy * sy;
		P[1][0][0] = (float)cx * 0.5f + (float)w->cMTx * sx;
		P[1][0][1] = 0.0f + (float)w->cMTy * sy;
		P[0][1][0] = 0.0f + (float)w->cMLx * sx;
		P[0][1][1] = (float)cy * 0.5f + (float)w->cMLy * sy;
		P[2][1][0] = (float)cx + (float)w->cMRx * sx;
		P[2][1][1] = (float)cy * 0.5f + (float)w->cMRy * sy;
		P[1][2][0] = (float)cx * 0.5f + (float)w->cMBx * sx;
		P[1][2][1] = (float)cy + (float)w->cMBy * sy;
		P[1][1][0] = (float)cx * 0.5f + (float)w->cCx * sx;
		P[1][1][1] = (float)cy * 0.5f + (float)w->cCy * sy;

		struct gs_vb_data *d = gs_vbdata_create();
		d->num = 24;
		d->points = (struct vec3 *)bmalloc(sizeof(struct vec3) * 24);
		d->num_tex = 1;
		d->tvarray = (struct gs_tvertarray *)bmalloc(sizeof(struct gs_tvertarray));
		d->tvarray[0].width = 2;
		d->tvarray[0].array = bmalloc(sizeof(struct vec2) * 24);
		struct vec2 *uvs = (struct vec2 *)d->tvarray[0].array;
		int vi = 0;
		for (int cj = 0; cj < 2; cj++) {
			for (int ci = 0; ci < 2; ci++) {
				const int order[6][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 0}, {1, 1}, {0, 1}};
				for (int k = 0; k < 6; k++) {
					const int px = ci + order[k][0];
					const int py = cj + order[k][1];
					d->points[vi].x = P[px][py][0];
					d->points[vi].y = P[px][py][1];
					d->points[vi].z = 0.0f;
					uvs[vi].x = (float)px * 0.5f;
					uvs[vi].y = (float)py * 0.5f;
					vi++;
				}
			}
		}
		obs_enter_graphics();
		w->curveVb = gs_vertexbuffer_create(d, GS_DYNAMIC);
		obs_leave_graphics();
		w->curveVbW = cx;
		w->curveVbH = cy;
		if (!w->curveVb) {
			return false;
		}
	}

	gs_viewport_push();
	gs_projection_push();
	gs_set_viewport(0, 0, (int)cx, (int)cy);
	gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
	gs_effect_set_float(gs_effect_get_param_by_name(eff, "gammaV"), (float)w->gammaV);
	gs_effect_set_float(gs_effect_get_param_by_name(eff, "liftV"), (float)w->liftV);
	gs_effect_set_float(gs_effect_get_param_by_name(eff, "gainV"), (float)w->gainV);
	gs_effect_set_texture_srgb(gs_effect_get_param_by_name(eff, "image"), tex);
	gs_enable_blending(false);
	gs_load_vertexbuffer(w->curveVb);
	gs_load_indexbuffer(nullptr);
	gs_technique_t *tech = gs_effect_get_technique(eff, "Draw");
	const size_t passes = gs_technique_begin(tech);
	for (size_t i = 0; i < passes; i++) {
		if (gs_technique_begin_pass(tech, i)) {
			gs_draw(GS_TRIS, 0, 24);
			gs_technique_end_pass(tech);
		}
	}
	gs_technique_end(tech);
	gs_load_vertexbuffer(nullptr);
	gs_enable_blending(true);
	gs_projection_pop();
	gs_viewport_pop();
	return true;
}
void HFRCanvasProjector::Draw(void *data, uint32_t cx, uint32_t cy)
{
	HFRCanvasProjector *window = static_cast<HFRCanvasProjector *>(data);
	if (!window || !window->ready || !window->canvas) {
		return;
	}
	struct obs_video_info ovi = {};
	if (!obs_canvas_get_video_info(window->canvas, &ovi) || !ovi.base_width || !ovi.base_height) {
		return;
	}
	const uint32_t cw = ovi.base_width;
	const uint32_t ch = ovi.base_height;

	/* 曲面网格：3x3 控制点网格 warp（同时叠加四角/色彩；与旋转暂不叠加） */
	if (window->hasTile && window->tileRot == 0 && window->hasCurve) {
		gs_texture_t *tex = obs_get_canvas_texture(window->canvas);
		if (tex && HFRCanvasProjector::DrawCurveMesh(tex, window, cx, cy)) {
			DrawBlendSafeOverlays(window, cx, cy);
			return;
		}
	}

	gs_viewport_push();
	gs_projection_push();

	/* 四角几何校正：整块用自定义 shader 映射四角（与旋转暂不叠加） */
	if (window->hasTile && window->tileRot == 0 && (window->hasKeystone || window->hasColorAdj)) {
		gs_texture_t *tex = obs_get_canvas_texture(window->canvas);
		if (tex) {
			const float sx = (float)cx / (float)std::max(1, window->tile.width());
			const float sy = (float)cy / (float)std::max(1, window->tile.height());
			gs_set_viewport(0, 0, (int)cx, (int)cy);
			gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
			if (DrawKeystoneQuad(tex, window, cx, cy)) {
				/* 边缘融合在 keystone 之后叠加（窗口坐标渐变，像素对齐） */
				if (window->blendL > 0) {
					DrawBlendGradient(0, 0.0f, 0.0f, (float)window->blendL * sx, (float)cy, 24);
				}
				if (window->blendR > 0) {
					DrawBlendGradient(1, (float)cx - (float)window->blendR * sx, 0.0f,
							  (float)window->blendR * sx, (float)cy, 24);
				}
				if (window->blendT > 0) {
					DrawBlendGradient(2, 0.0f, 0.0f, (float)cx, (float)window->blendT * sy, 24);
				}
				if (window->blendB > 0) {
					DrawBlendGradient(3, 0.0f, (float)cy - (float)window->blendB * sy, (float)cx,
							  (float)window->blendB * sy, 24);
				}
				gs_projection_pop();
				gs_viewport_pop();
				/* 安全区在 keystone 之上（与普通路径一致） */
				if (window->HasSafeArea()) {
					gs_viewport_push();
					gs_projection_push();
					gs_set_viewport(0, 0, (int)cx, (int)cy);
					gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
					DrawSafeAreaRect(cx, cy, 0.05f, 0x66FFFFFF);
					DrawSafeAreaRect(cx, cy, 0.10f, 0x66FF4040);
					gs_projection_pop();
					gs_viewport_pop();
				}
				return;
			}
		}
	}

	if (window->hasTile) {
		const QRect &t = window->tile;
		const int rot = window->tileRot;
		const float tw = (float)t.width();
		const float th = (float)t.height();
		const float halfW = (rot == 90 || rot == 270) ? th * 0.5f : tw * 0.5f;
		const float halfH = (rot == 90 || rot == 270) ? tw * 0.5f : th * 0.5f;
		gs_set_viewport(0, 0, (int)cx, (int)cy);
		gs_ortho(-halfW, halfW, -halfH, halfH, -100.0f, 100.0f);
		if (rot) {
			gs_matrix_push();
			gs_matrix_identity();
			gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, (float)rot * 3.14159265f / 180.0f);
			gs_matrix_translate3f(-(float)t.x() - tw * 0.5f, -(float)t.y() - th * 0.5f, 0.0f);
			obs_render_canvas_texture(window->canvas);
			gs_matrix_pop();
			gs_projection_pop();
			gs_viewport_pop();
			if (window->HasSafeArea()) {
				gs_viewport_push();
				gs_projection_push();
				gs_set_viewport(0, 0, (int)cx, (int)cy);
				gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
				DrawSafeAreaRect(cx, cy, 0.05f, 0x66FFFFFF);
				DrawSafeAreaRect(cx, cy, 0.10f, 0x66FF4040);
				gs_projection_pop();
				gs_viewport_pop();
			}
			return;
		}
		gs_ortho((float)t.x(), (float)(t.x() + t.width()), (float)t.y(),
			 (float)(t.y() + t.height()), -100.0f, 100.0f);
	} else {
		/* 非固定分辨率：整幅画布等比缩放居中（letterbox），任意画布尺寸都能正确定屏 */
		const float scale = std::min((float)cx / (float)cw, (float)cy / (float)ch);
		uint32_t nw = (uint32_t)(scale * (float)cw);
		uint32_t nh = (uint32_t)(scale * (float)ch);
		if (!nw || !nh) {
			nw = cx;
			nh = cy;
		}
		gs_set_viewport((int)((cx - nw) / 2), (int)((cy - nh) / 2), (int)nw, (int)nh);
		gs_ortho(0.0f, (float)cw, 0.0f, (float)ch, -100.0f, 100.0f);
	}
	obs_render_canvas_texture(window->canvas);

	/* 边缘融合：仅在轴对齐分块上生效（旋转块暂不融合） */
	if (window->hasTile && window->tileRot == 0 &&
	    (window->blendL > 0 || window->blendR > 0 || window->blendT > 0 || window->blendB > 0)) {
		const QRect &t = window->tile;
		if (window->blendL > 0) {
			DrawBlendGradient(0, (float)t.x(), (float)t.y(), (float)window->blendL,
					  (float)t.height(), 24);
		}
		if (window->blendR > 0) {
			DrawBlendGradient(1, (float)(t.x() + t.width() - window->blendR), (float)t.y(),
					  (float)window->blendR, (float)t.height(), 24);
		}
		if (window->blendT > 0) {
			DrawBlendGradient(2, (float)t.x(), (float)t.y(), (float)t.width(),
					  (float)window->blendT, 24);
		}
		if (window->blendB > 0) {
			DrawBlendGradient(3, (float)t.x(), (float)(t.y() + t.height() - window->blendB),
					  (float)t.width(), (float)window->blendB, 24);
		}
	}
	gs_projection_pop();
	gs_viewport_pop();

	if (window->HasSafeArea()) {
		gs_viewport_push();
		gs_projection_push();
		gs_set_viewport(0, 0, (int)cx, (int)cy);
		gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
		DrawSafeAreaRect(cx, cy, 0.05f, 0x66FFFFFF); /* 90% */
		DrawSafeAreaRect(cx, cy, 0.10f, 0x66FF4040); /* 80% */
		gs_projection_pop();
		gs_viewport_pop();
	}
}

/* --------------------------- HFRConsoleDock --------------------------- */

/* ---------------- 前端热键（远程/中控联动） ---------------- */

template<typename Fn> static void RunOnDockUi(HFRConsoleDock *dock, Fn fn)
{
	if (!dock) {
		return;
	}
	QMetaObject::invokeMethod(dock, [dock, fn]() { fn(dock); }, Qt::QueuedConnection);
}

/* 前置声明：场景枚举回调（定义在下方） */
static bool CollectSceneSourcesCallback(void *param, obs_source_t *sceneSource);

static void HotkeyCueNext(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed) {
		RunOnDockUi(static_cast<HFRConsoleDock *>(data), [](HFRConsoleDock *d) { d->NextCue(); });
	}
}

static void HotkeyCuePrev(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed) {
		RunOnDockUi(static_cast<HFRConsoleDock *>(data), [](HFRConsoleDock *d) { d->PrevCue(); });
	}
}

static void HotkeyCueRunCurrent(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed) {
		RunOnDockUi(static_cast<HFRConsoleDock *>(data), [](HFRConsoleDock *d) { d->ExecuteCurrentCue(); });
	}
}

static void HotkeyCueAuto(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed) {
		RunOnDockUi(static_cast<HFRConsoleDock *>(data), [](HFRConsoleDock *d) { d->ToggleAutoPlay(); });
	}
}

static void HotkeyBlackout(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed) {
		RunOnDockUi(static_cast<HFRConsoleDock *>(data), [](HFRConsoleDock *d) { d->ToggleBlackout(); });
	}
}

static void HotkeyProjectPrimary(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed) {
		RunOnDockUi(static_cast<HFRConsoleDock *>(data), [](HFRConsoleDock *d) { d->ProjectSelectedToPrimary(); });
	}
}

static void HotkeyStopProjections(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed) {
		RunOnDockUi(static_cast<HFRConsoleDock *>(data), [](HFRConsoleDock *d) { d->CloseAllProjectors(); });
	}
}

static void HotkeyRecStart(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed) {
		RunOnDockUi(static_cast<HFRConsoleDock *>(data), [](HFRConsoleDock *d) { d->StartRecordingSelected(); });
	}
}

static void HotkeyRecStop(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed) {
		RunOnDockUi(static_cast<HFRConsoleDock *>(data), [](HFRConsoleDock *d) { d->StopRecordingSelected(); });
	}
}

void HFRConsoleDock::ExecuteCurrentCue()
{
	const int row = cueList ? cueList->currentRow() : -1;
	if (row < 0) {
		SetStatus(QStringLiteral("节目单：未选中条目"), 1);
		return;
	}
	ExecuteCue(row);
}

void HFRConsoleDock::ProjectSelectedToPrimary()
{
	obs_canvas_t *cv = CanvasFromRow(canvasList->currentRow());
	if (!cv) {
		SetStatus(QStringLiteral("请先点选画布"), 1);
		return;
	}
	ProjectCanvasToMonitor(cv, 0); /* 0 号显示器 = 主屏 */
}

/* ---------------- 画布级动态热键（远程 list/switch/project 语义） ---------------- */

static void HfrCanvasHotkeyCb(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed || !data) {
		return;
	}
	auto *d = static_cast<HfrCanvasHotkeyData *>(data);
	if (!d || !d->dock) {
		return;
	}
	HFRConsoleDock *dock = d->dock;
	const int row = d->canvasRow;
	const int kind = d->kind;
	const int arg = d->arg;
	QMetaObject::invokeMethod(
		dock,
		[dock, row, kind, arg]() {
			if (kind == 0) {
				dock->CanvasActivateProject(row, arg);
			} else {
				dock->CanvasActivateScene(row, arg);
			}
		},
		Qt::QueuedConnection);
}

void HFRConsoleDock::UnregisterCanvasHotkeys()
{
	for (obs_hotkey_id id : canvasHotkeyIds) {
		obs_hotkey_unregister(id);
	}
	canvasHotkeyIds.clear();
	for (HfrCanvasHotkeyData *d : canvasHotkeyData) {
		delete d;
	}
	canvasHotkeyData.clear();
}

void HFRConsoleDock::ApplyDefaultHotkeys()
{
	struct Ent {
		obs_hotkey_id id;
		obs_key_t key;
		const char *desc;
	};
	const Ent ents[] = {
		{hkCueNext, OBS_KEY_1, "节目单下一条"},
		{hkCuePrev, OBS_KEY_2, "节目单上一条"},
		{hkCueRun, OBS_KEY_3, "执行当前节目单条目"},
		{hkCueAuto, OBS_KEY_4, "自动播放 开/关"},
		{hkBlackout, OBS_KEY_5, "黑场 开/关"},
		{hkProjectPrimary, OBS_KEY_6, "当前画布投影到主屏"},
		{hkStopProjections, OBS_KEY_7, "关闭全部投影"},
		{hkRecStart, OBS_KEY_8, "录制当前画布"},
		{hkRecStop, OBS_KEY_9, "停止录制当前画布"},
	};
	for (const Ent &e : ents) {
		if (e.id == OBS_INVALID_HOTKEY_ID) {
			continue;
		}
		obs_key_combination_t c = {};
		/* INTERACT_CONTROL_KEY(1<<2) | INTERACT_ALT_KEY(1<<3) */
		c.modifiers = (1u << 2) | (1u << 3);
		c.key = e.key;
		obs_hotkey_load_bindings(e.id, &c, 1);
		obs_hotkey_save(e.id);
	}
	SetStatus(QStringLiteral("已应用默认热键：Ctrl+Alt+1..9（可在 设置→热键 中修改或清除）"));
}

void HFRConsoleDock::RegisterCanvasHotkeys()
{
	UnregisterCanvasHotkeys();
	if (!canvasList) {
		return;
	}
	const int rows = std::min(canvasList->count(), 4); /* 最多 4 个画布 */
	const auto screens = QGuiApplication::screens();
	const int maxScreens = std::min((int)screens.size(), 4);
	for (int row = 0; row < rows; row++) {
		for (int s = 0; s < maxScreens; s++) {
			auto *d = new HfrCanvasHotkeyData;
			d->dock = this;
			d->canvasRow = row;
			d->kind = 0;
			d->arg = s;
			const QString name = QStringLiteral("hfr.canvas.%1.project_screen.%2").arg(row).arg(s);
			const QString desc =
				QStringLiteral("HFR: 画布%1 → 投影到屏%2（远程）").arg(row).arg(s);
			canvasHotkeyIds.push_back(obs_hotkey_register_frontend(name.toUtf8().constData(),
									     desc.toUtf8().constData(),
									     HfrCanvasHotkeyCb, d));
			canvasHotkeyData.push_back(d);
		}
		for (int k = 1; k <= 4; k++) {
			auto *d = new HfrCanvasHotkeyData;
			d->dock = this;
			d->canvasRow = row;
			d->kind = 1;
			d->arg = k;
			const QString name = QStringLiteral("hfr.canvas.%1.scene.%2").arg(row).arg(k);
			const QString desc =
				QStringLiteral("HFR: 画布%1 → 切到第%2个场景（远程）").arg(row).arg(k);
			canvasHotkeyIds.push_back(obs_hotkey_register_frontend(name.toUtf8().constData(),
									     desc.toUtf8().constData(),
									     HfrCanvasHotkeyCb, d));
			canvasHotkeyData.push_back(d);
		}
	}
	HfrLog("canvas hotkeys registered");
}

void HFRConsoleDock::CanvasActivateProject(int canvasRow, int screenIndex)
{
	obs_canvas_t *cv = CanvasFromRow(canvasRow);
	if (!cv) {
		SetStatus(QStringLiteral("远程投影：画布 %1 不存在").arg(canvasRow), 1);
		return;
	}
	ProjectCanvasToMonitor(cv, screenIndex);
}

void HFRConsoleDock::CanvasActivateScene(int canvasRow, int sceneOrdinal)
{
	obs_canvas_t *cv = CanvasFromRow(canvasRow);
	if (!cv) {
		SetStatus(QStringLiteral("远程切场景：画布 %1 不存在").arg(canvasRow), 1);
		return;
	}
	std::vector<obs_source_t *> scenes;
	obs_canvas_enum_scenes(cv, CollectSceneSourcesCallback, &scenes);
	const int idx = sceneOrdinal - 1;
	if (idx < 0 || idx >= (int)scenes.size()) {
		SetStatus(QStringLiteral("远程切场景：画布 %1 没有第 %2 个场景").arg(canvasRow).arg(sceneOrdinal), 1);
		return;
	}
	obs_canvas_set_channel(cv, 0, scenes[(size_t)idx]);
	if (CanvasFromRow(canvasList ? canvasList->currentRow() : -1) == cv) {
		UpdateSceneListForSelection();
	}
	SaveState();
	SetStatus(QStringLiteral("远程：画布 %1 → 场景「%2」")
			  .arg(canvasRow)
			  .arg(QString::fromUtf8(obs_source_get_name(scenes[(size_t)idx]))));
}

void HFRConsoleDock::RegisterHfrHotkeys()
{
	hkCueNext = obs_hotkey_register_frontend("hfr.cue_next", "HFR: 节目单下一条", HotkeyCueNext, this);
	hkCuePrev = obs_hotkey_register_frontend("hfr.cue_prev", "HFR: 节目单上一条", HotkeyCuePrev, this);
	hkCueRun = obs_hotkey_register_frontend("hfr.cue_run_current", "HFR: 执行当前节目单条目",
						HotkeyCueRunCurrent, this);
	hkCueAuto = obs_hotkey_register_frontend("hfr.cue_auto_toggle", "HFR: 自动播放 开/关", HotkeyCueAuto,
						 this);
	hkBlackout = obs_hotkey_register_frontend("hfr.blackout_toggle", "HFR: 黑场 开/关", HotkeyBlackout,
						  this);
	hkProjectPrimary = obs_hotkey_register_frontend("hfr.project_primary", "HFR: 当前画布投影到主屏",
							HotkeyProjectPrimary, this);
	hkStopProjections = obs_hotkey_register_frontend("hfr.stop_all_projections", "HFR: 关闭全部投影",
							 HotkeyStopProjections, this);
	hkRecStart = obs_hotkey_register_frontend("hfr.rec_start", "HFR: 录制当前画布", HotkeyRecStart, this);
	hkRecStop = obs_hotkey_register_frontend("hfr.rec_stop", "HFR: 停止录制当前画布", HotkeyRecStop, this);
	HfrLog("hfr hotkeys registered");
}

HFRConsoleDock::HFRConsoleDock(QWidget *parent) : QDockWidget(parent)
{
	s_instance = this;
	setWindowTitle(QStringLiteral("输出矩阵（画布）"));
	setObjectName(QStringLiteral("hfrOutputMatrixDock"));

	BuildUi();
	WireSignals();

	RefreshMonitors();
	RefreshCanvasList();
	UpdateSceneListForSelection();

	LoadState();
	UpdateSceneListForSelection();
	RegisterHfrHotkeys();

	/* 无人值守自检：设置 HFR_PPT_AUTOTEST=<pptx 路径> 后启动即渲染一次并写日志 */
	if (qEnvironmentVariableIsSet("HFR_PPT_AUTOTEST")) {
		const QString pptPath = qEnvironmentVariable("HFR_PPT_AUTOTEST");
		QTimer::singleShot(5000, [pptPath]() {
			QString err;
			HFRPptDocument &doc = PptSelfTestDoc();
			blog(LOG_INFO, "[HFR-PPT] autotest: open %s", pptPath.toUtf8().constData());
			if (!doc.Open(pptPath, &err)) {
				blog(LOG_WARNING, "[HFR-PPT] autotest open failed: %s", err.toUtf8().constData());
				return;
			}
			std::vector<uint8_t> buf;
			if (doc.RenderCurrentPart(1280, 720, buf, &err)) {
				const QString out = HfrPptTestOutputDir() + QStringLiteral("/hfr_ppt_autotest.bmp");
				WriteBmp32File(out, 1280, 720, buf);
				blog(LOG_INFO, "[HFR-PPT] autotest OK: 第 %d/%d 页 -> %s", doc.CurrentPart() + 1,
				     doc.PartCount(), out.toUtf8().constData());
			} else {
				blog(LOG_WARNING, "[HFR-PPT] autotest render failed: %s", err.toUtf8().constData());
			}
		});
	}
}

HFRConsoleDock::~HFRConsoleDock()
{
	CleanupForShutdown();
	s_instance = nullptr;
}

/* 往指定场景加入一块按画布分辨率铺满的色块 */
static void AddColorIntoScene(obs_canvas_t *cv, obs_scene_t *sc, const QString &srcName)
{
	if (!cv || !sc) {
		return;
	}
	struct obs_video_info ovi = {};
	uint32_t w = 1280, h = 720;
	if (obs_canvas_get_video_info(cv, &ovi) && ovi.base_width && ovi.base_height) {
		w = ovi.base_width;
		h = ovi.base_height;
	}
	obs_data_t *s = obs_data_create();
	obs_data_set_int(s, "color", 0xFF00C8FF);
	obs_data_set_int(s, "width", (long long)w);
	obs_data_set_int(s, "height", (long long)h);
	obs_source_t *cs = obs_source_create("color_source_v3", srcName.toUtf8().constData(), s, nullptr);
	obs_data_release(s);
	if (!cs) {
		return;
	}
	obs_sceneitem_t *it = obs_scene_add(sc, cs);
	if (it) {
		obs_sceneitem_set_bounds_type(it, OBS_BOUNDS_STRETCH);
		struct vec2 sz = {(float)w, (float)h};
		obs_sceneitem_set_bounds(it, &sz);
		struct vec2 pos = {0.0f, 0.0f};
		obs_sceneitem_set_pos(it, &pos);
	}
	obs_source_release(cs);
}

/* 枚举回调：收集画布内场景（先收集后移除，避免遍历中修改） */
static bool CollectSceneCallback(void *param, obs_source_t *sceneSource)
{
	auto *scenes = static_cast<std::vector<obs_scene_t *> *>(param);
	if (obs_scene_t *sc = obs_scene_from_source(sceneSource)) {
		scenes->push_back(sc);
	}
	return true;
}

/* 运行时删除单个画布：清通道 → 移除其场景 → 释放画布 */
static void TeardownUserCanvas(obs_canvas_t *cv)
{
	if (!cv || cv == obs_get_main_canvas()) {
		return;
	}
	obs_canvas_set_channel(cv, 0, nullptr);
	std::vector<obs_scene_t *> scenes;
	obs_canvas_enum_scenes(cv, CollectSceneCallback, &scenes);
	for (obs_scene_t *sc : scenes) {
		obs_canvas_scene_remove(sc);
	}
	obs_canvas_remove(cv);
	obs_canvas_release(cv);
}

void HFRConsoleDock::CleanupForShutdown()
{
	if (countdownTimer) {
		countdownTimer->stop();
	}
	for (auto it = countdowns.begin(); it != countdowns.end(); ++it) {
		obs_source_release(it.key());
	}
	countdowns.clear();
	/* 独立播放器/讲者视图窗口在 OBS 存活期内销毁（释放内部来源与纹理） */
	HfrShutdownPptPlayerUi();
	HfrShutdownPresenter();
	/* 画布级热键注销（回调上下文在注销时释放） */
	UnregisterCanvasHotkeys();
	/* 解说源混音恢复 */
	RestoreCommentatorMixers();
	/* 黑场前的通道场景引用（若黑场中退出） */
	for (auto it = blackoutPrev.begin(); it != blackoutPrev.end(); ++it) {
		if (it.value()) {
			obs_source_release(it.value());
		}
	}
	blackoutPrev.clear();
	blackoutActive = false;
	StopAllRecordings();
	CloseAllProjectors();
	for (obs_canvas_t *cv : canvases) {
		TeardownUserCanvas(cv);
	}
	canvases.clear();
	testSceneAdded.clear();
	safeAreaPref.clear();

	/* 关键：在 obs_shutdown 之前把延迟销毁队列排空（官方 API），
	 * 否则画布场景/源的销毁会与 obs_free_data 竞争（曾致 c0000005） */
	obs_wait_for_destroy_queue();
	obs_wait_for_destroy_queue();
	HfrLog("cleanup: scenes removed, canvases released, destroy queue drained");
}

void HFRConsoleDock::RefreshMonitors()
{
	monitorList->clear();
	const auto screens = QGuiApplication::screens();
	for (int i = 0; i < screens.size(); i++) {
		QScreen *s = screens.at(i);
		QString label = QStringLiteral("#%1  %2  %3x%4%5")
					.arg(i)
					.arg(s->name())
					.arg(s->geometry().width())
					.arg(s->geometry().height())
					.arg(s == QGuiApplication::primaryScreen() ? QStringLiteral("  [主]") : QString());
		monitorList->addItem(label);
	}
	status->setText(QStringLiteral("显示器 %1 块；接好外接屏后点“刷新”重新编号").arg(screens.size()));
}

void HFRConsoleDock::AddCanvasRowItem(obs_canvas_t *cv, const QString &label)
{
	Q_UNUSED(label);
	QListWidgetItem *item = new QListWidgetItem(canvasList);
	item->setData(Qt::UserRole, QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(cv)));
	HFRCanvasRow *row = new HFRCanvasRow(cv, recordings.contains(cv), canvasList);
	item->setSizeHint(row->sizeHint());
	canvasList->setItemWidget(item, row);
}

void HFRConsoleDock::RefreshCanvasList()
{
	canvasList->clear();
	obs_canvas_t *mainCanvas = obs_get_main_canvas();
	if (mainCanvas) {
		AddCanvasRowItem(mainCanvas, QString());
		obs_canvas_release(mainCanvas);
	}
	for (obs_canvas_t *cv : canvases) {
		if (cv) {
			AddCanvasRowItem(cv, QString());
		}
	}
	if (canvasList->count() > 0) {
		canvasList->setCurrentRow(0);
	}
	/* 画布/场景变化后刷新画布级远程热键 */
	RegisterCanvasHotkeys();
}

obs_canvas_t *HFRConsoleDock::CanvasFromRow(int row) const
{
	QListWidgetItem *item = canvasList->item(row);
	if (!item) {
		return nullptr;
	}
	return reinterpret_cast<obs_canvas_t *>(
		static_cast<quintptr>(item->data(Qt::UserRole).toULongLong()));
}

void HFRConsoleDock::CreateCanvas()
{
	bool okName = false;
	QString name = QInputDialog::getText(this, QStringLiteral("新建画布"),
					     QStringLiteral("画布名称"), QLineEdit::Normal,
					     QStringLiteral("画布"), &okName);
	if (!okName || name.trimmed().isEmpty()) {
		return;
	}

	const QStringList resList = {QStringLiteral("1920x1080"), QStringLiteral("1280x720"),
				     QStringLiteral("960x540"), QStringLiteral("2560x1440"),
				     QStringLiteral("3840x1080（双屏拼接墙）"),
				     QStringLiteral("1080x1920（竖屏）"),
				     QStringLiteral("自定义…")};
	bool okRes = false;
	QString res = QInputDialog::getItem(this, QStringLiteral("新建画布"),
					    QStringLiteral("基础分辨率（= 拼接墙分辨率；帧率随全局）"),
					    resList, 0, false, &okRes);
	if (!okRes || res.isEmpty()) {
		return;
	}
	uint32_t bw = 0, bh = 0;
	if (res == QStringLiteral("自定义…")) {
		bool okW = false, okH = false;
		const int w = QInputDialog::getInt(this, QStringLiteral("自定义分辨率"),
						   QStringLiteral("宽度（像素）"), 1920, 64, 16384, 2, &okW);
		const int h = QInputDialog::getInt(this, QStringLiteral("自定义分辨率"),
						   QStringLiteral("高度（像素）"), 1080, 64, 16384, 2, &okH);
		if (!okW || !okH) {
			return;
		}
		bw = (uint32_t)w;
		bh = (uint32_t)h;
	} else {
		QStringList wh = res.split(QLatin1Char('x'));
		if (wh.size() != 2) {
			return;
		}
		QString w0 = wh.at(0), h0 = wh.at(1);
		w0 = w0.split(QStringLiteral("（")).first().trimmed();
		h0 = h0.split(QStringLiteral("（")).first().trimmed();
		bw = w0.toUInt();
		bh = h0.toUInt();
	}
	if (!bw || !bh) {
		return;
	}

	struct obs_video_info ovi = {};
	obs_get_video_info(&ovi);
	ovi.base_width = bw;
	ovi.base_height = bh;

	obs_canvas_t *cv = obs_canvas_create(name.trimmed().toUtf8().constData(), &ovi, ACTIVATE | SCENE_REF);
	if (!cv) {
		SetStatus(QStringLiteral("新建画布失败"), 2);
		return;
	}
	canvases.append(cv);
	RefreshCanvasList();
	canvasList->setCurrentRow(canvasList->count() - 1);
	SaveState();
	HfrLog("created canvas with chosen name/resolution");
	status->setText(QStringLiteral("已新建画布：%1（点“填充测试色”即可见内容）")
				.arg(QString::fromUtf8(obs_canvas_get_name(cv))));
}

void HFRConsoleDock::RenameSelectedCanvas()
{
	int row = canvasList->currentRow();
	obs_canvas_t *cv = CanvasFromRow(row);
	if (!cv) {
		SetStatus(QStringLiteral("请先点选画布"), 1);
		return;
	}
	if (cv == obs_get_main_canvas()) {
		SetStatus(QStringLiteral("主画布不可重命名"), 1);
		return;
	}
	bool ok = false;
	QString name = QInputDialog::getText(this, QStringLiteral("重命名画布"),
					      QStringLiteral("画布名称"),
					      QLineEdit::Normal,
					      QString::fromUtf8(obs_canvas_get_name(cv)), &ok);
	if (!ok || name.trimmed().isEmpty()) {
		return;
	}
	obs_canvas_set_name(cv, name.trimmed().toUtf8().constData());
	RefreshCanvasList();
	SaveState();
	HfrLog("renamed canvas");
}

/* ---------------- 场景作用域（当前画布） ---------------- */

static bool CollectSceneSourcesCallback(void *param, obs_source_t *sceneSource)
{
	auto *vec = static_cast<std::vector<obs_source_t *> *>(param);
	vec->push_back(sceneSource);
	return true;
}

void HFRConsoleDock::UpdateSceneListForSelection()
{
	if (!sceneList) {
		return;
	}
	sceneList->clear();
	obs_canvas_t *cv = CanvasFromRow(canvasList->currentRow());
	if (!cv) {
		return;
	}
	std::vector<obs_source_t *> scenes;
	obs_canvas_enum_scenes(cv, CollectSceneSourcesCallback, &scenes);

	obs_source_t *cur = obs_canvas_get_channel(cv, 0);
	QString curName = cur ? QString::fromUtf8(obs_source_get_name(cur)) : QString();

	for (obs_source_t *s : scenes) {
		const QString name = QString::fromUtf8(obs_source_get_name(s));
		QListWidgetItem *item = new QListWidgetItem(
			(name == curName ? QStringLiteral("▶ ") : QStringLiteral("   ")) + name, sceneList);
		item->setData(Qt::UserRole, QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(s)));
		if (name == curName) {
			sceneList->setCurrentItem(item);
		}
	}
	/* 切换台与场景列表保持同步（作用域画布/按钮高亮） */
	if (switcher) {
		switcher->SetCanvas(cv);
	}
}

void HFRConsoleDock::CreateSceneForSelected()
{
	obs_canvas_t *cv = CanvasFromRow(canvasList->currentRow());
	if (!cv) {
		SetStatus(QStringLiteral("请先点选画布"), 1);
		return;
	}
	bool ok = false;
	QString name = QInputDialog::getText(this, QStringLiteral("新建场景"), QStringLiteral("场景名称"),
					     QLineEdit::Normal,
					     QString::fromUtf8(obs_canvas_get_name(cv)) + QStringLiteral(" · 场景"), &ok);
	if (!ok || name.trimmed().isEmpty()) {
		return;
	}
	obs_scene_t *sc = obs_canvas_scene_create(cv, name.trimmed().toUtf8().constData());
	if (!sc) {
		SetStatus(QStringLiteral("创建场景失败（名称冲突？）"), 2);
		return;
	}
	UpdateSceneListForSelection();
	SaveState();
	status->setText(QStringLiteral("已新建场景：%1").arg(name.trimmed()));
}

void HFRConsoleDock::SwitchToSelectedScene()
{
	obs_canvas_t *cv = CanvasFromRow(canvasList->currentRow());
	if (!cv) {
		SetStatus(QStringLiteral("请先点选画布"), 1);
		return;
	}
	QListWidgetItem *item = sceneList->currentItem();
	if (!item) {
		SetStatus(QStringLiteral("请先选中场景"), 1);
		return;
	}
	obs_source_t *scSrc = reinterpret_cast<obs_source_t *>(
		static_cast<quintptr>(item->data(Qt::UserRole).toULongLong()));
	if (!scSrc) {
		return;
	}
	obs_canvas_set_channel(cv, 0, scSrc);
	UpdateSceneListForSelection();
	SaveState();
	status->setText(QStringLiteral("已切换场景：%1").arg(QString::fromUtf8(obs_source_get_name(scSrc))));
	HfrLog("scene switched on canvas");
}

void HFRConsoleDock::DeleteSelectedScene()
{
	obs_canvas_t *cv = CanvasFromRow(canvasList->currentRow());
	QListWidgetItem *item = sceneList->currentItem();
	if (!cv || !item) {
		SetStatus(QStringLiteral("请先选中画布与场景"), 1);
		return;
	}
	obs_source_t *scSrc = reinterpret_cast<obs_source_t *>(
		static_cast<quintptr>(item->data(Qt::UserRole).toULongLong()));
	obs_scene_t *sc = scSrc ? obs_scene_from_source(scSrc) : nullptr;
	if (!sc) {
		return;
	}
	/* 若删除的是当前场景，先断开通道，避免引用悬空 */
	if (obs_canvas_get_channel(cv, 0) == scSrc) {
		obs_canvas_set_channel(cv, 0, nullptr);
	}
	obs_canvas_scene_remove(sc);
	UpdateSceneListForSelection();
	SaveState();
	status->setText(QStringLiteral("已删除场景"));
}

void HFRConsoleDock::FillColorIntoSelectedScene()
{
	obs_canvas_t *cv = CanvasFromRow(canvasList->currentRow());
	QListWidgetItem *item = sceneList->currentItem();
	if (!cv || !item) {
		SetStatus(QStringLiteral("请先选中画布与场景"), 1);
		return;
	}
	obs_source_t *scSrc = reinterpret_cast<obs_source_t *>(
		static_cast<quintptr>(item->data(Qt::UserRole).toULongLong()));
	obs_scene_t *sc = scSrc ? obs_scene_from_source(scSrc) : nullptr;
	if (!sc) {
		return;
	}

	AddColorIntoScene(cv, sc, QString::fromUtf8(obs_source_get_name(scSrc)) + QStringLiteral(" · 色块"));
	status->setText(QStringLiteral("已给场景加色块"));
	HfrLog("color added into canvas scene");
}

/* ---------------- 节目单编排（Rundown） ---------------- */

static obs_canvas_t *FindCanvasByNameForCue(const QVector<obs_canvas_t *> &canvases, const QString &name,
					    bool *isMain = nullptr)
{
	if (isMain) {
		*isMain = false;
	}
	obs_canvas_t *main = obs_get_main_canvas();
	if (main) {
		if (name == QString::fromUtf8(obs_canvas_get_name(main))) {
			if (isMain) {
				*isMain = true;
			}
			return main; /* 调用方负责 release */
		}
		obs_canvas_release(main);
	}
	for (obs_canvas_t *cv : canvases) {
		if (cv && name == QString::fromUtf8(obs_canvas_get_name(cv))) {
			return cv;
		}
	}
	return nullptr;
}

static obs_source_t *FindSceneSourceByNameForCue(obs_canvas_t *cv, const QString &scene)
{
	if (!cv) {
		return nullptr;
	}
	std::vector<obs_source_t *> scenes;
	obs_canvas_enum_scenes(cv, CollectSceneSourcesCallback, &scenes);
	for (obs_source_t *s : scenes) {
		if (s && scene == QString::fromUtf8(obs_source_get_name(s))) {
			return s;
		}
	}
	return nullptr;
}

void HFRConsoleDock::AddSpecialCue(int type)
{
	if (type == 1 || type == 2) {
		if (!HfrFindFirstSlidesSource()) {
			SetStatus(QStringLiteral("没有幻灯片来源：先在场景里添加「幻灯片（自研引擎）」"), 1);
			return;
		}
	}
	HfrCue c;
	c.type = type;
	cues.push_back(c);
	RefreshCueList();
	if (cueList) {
		cueList->setCurrentRow(cueList->count() - 1);
	}
	SaveState();
	const char *names[] = {"场景", "幻灯片下一页", "幻灯片上一页", "黑场", "恢复画面"};
	SetStatus(QStringLiteral("节目单追加：%1").arg(QString::fromUtf8(type >= 0 && type <= 4 ? names[type] : "条目")));
}

void HFRConsoleDock::AddDelayCue(int seconds)
{
	const int sec = std::max(1, std::min(3600, seconds));
	HfrCue c;
	c.type = 5;
	c.waitMs = sec * 1000;
	cues.push_back(c);
	RefreshCueList();
	if (cueList) {
		cueList->setCurrentRow(cueList->count() - 1);
	}
	SaveState();
	SetStatus(QStringLiteral("节目单追加：延时 %1 秒").arg(sec));
}

void HFRConsoleDock::AddScopeCue()
{
	obs_canvas_t *cv = CanvasFromRow(canvasList ? canvasList->currentRow() : -1);
	if (!cv) {
		SetStatus(QStringLiteral("请先点选画布，再加入「切换操作画布」"), 1);
		return;
	}
	HfrCue c;
	c.type = 6;
	c.canvas = QString::fromUtf8(obs_canvas_get_name(cv));
	cues.push_back(c);
	RefreshCueList();
	if (cueList) {
		cueList->setCurrentRow(cueList->count() - 1);
	}
	SaveState();
	SetStatus(QStringLiteral("节目单追加：切换操作画布 → %1").arg(c.canvas));
}

void HFRConsoleDock::RefreshCueList()
{
	if (!cueList) {
		return;
	}
	cueList->clear();
	for (int i = 0; i < cues.size(); i++) {
		const HfrCue &c = cues.at(i);
		QString text;
		switch (c.type) {
		case 1:
			text = QStringLiteral("%1. [幻灯片] 下一页").arg(i + 1);
			break;
		case 2:
			text = QStringLiteral("%1. [幻灯片] 上一页").arg(i + 1);
			break;
		case 3:
			text = QStringLiteral("%1. [黑场]").arg(i + 1);
			break;
		case 4:
			text = QStringLiteral("%1. [恢复画面]").arg(i + 1);
			break;
		case 5:
			text = QStringLiteral("%1. [延时 %2 秒]").arg(i + 1).arg(c.waitMs / 1000.0, 0, 'f', 1);
			break;
		case 6:
			text = QStringLiteral("%1. [操作画布] %2").arg(i + 1).arg(c.canvas);
			break;
		default:
			text = QStringLiteral("%1. %2 → %3").arg(i + 1).arg(c.canvas, c.scene);
			break;
		}
		if (i == cueIndex) {
			text = QStringLiteral("▶ ") + text;
		}
		cueList->addItem(text);
	}
	if (cueIndex >= 0 && cueIndex < cueList->count()) {
		cueList->setCurrentRow(cueIndex);
	}
}

void HFRConsoleDock::ExportRundown()
{
	if (cues.isEmpty()) {
		SetStatus(QStringLiteral("节目单为空：没有可导出的条目"), 1);
		return;
	}
	QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出节目单"),
						    QStringLiteral("hfr_rundown.json"),
						    QStringLiteral("节目单文件 (*.json)"));
	if (path.isEmpty()) {
		return;
	}
	if (!path.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) {
		path += QStringLiteral(".json");
	}
	QJsonArray arr;
	for (const HfrCue &c : cues) {
		QJsonObject o;
		o[QStringLiteral("type")] = c.type;
		o[QStringLiteral("waitMs")] = c.waitMs;
		o[QStringLiteral("canvas")] = c.canvas;
		o[QStringLiteral("scene")] = c.scene;
		arr.append(o);
	}
	QFile f(path);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		SetStatus(QStringLiteral("导出失败：无法写入 %1（%2）").arg(QFileInfo(path).fileName(), f.errorString()), 2);
		return;
	}
	const qint64 written = f.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
	f.close();
	if (written <= 0) {
		SetStatus(QStringLiteral("导出失败：写入 %1 时出错").arg(QFileInfo(path).fileName()), 2);
		return;
	}
	SetStatus(QStringLiteral("已导出节目单 %1 条 → %2").arg(arr.size()).arg(QFileInfo(path).fileName()));
	HfrLog("rundown exported");
}

void HFRConsoleDock::ImportRundown()
{
	const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("导入节目单"), QString(),
							  QStringLiteral("节目单文件 (*.json);;所有文件 (*)"));
	if (path.isEmpty()) {
		return;
	}
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly)) {
		SetStatus(QStringLiteral("导入失败：无法读取 %1（%2）").arg(QFileInfo(path).fileName(), f.errorString()), 2);
		return;
	}
	const QByteArray data = f.readAll();
	f.close();
	QJsonParseError parseErr = {};
	const QJsonDocument doc = QJsonDocument::fromJson(data, &parseErr);
	if (parseErr.error != QJsonParseError::NoError) {
		SetStatus(QStringLiteral("导入失败：JSON 解析错误（%1）").arg(parseErr.errorString()), 2);
		return;
	}
	if (!doc.isArray()) {
		SetStatus(QStringLiteral("导入失败：格式应为 [{\"canvas\":\"画布名\",\"scene\":\"场景名\"}] 数组"), 2);
		return;
	}
	QVector<HfrCue> imported;
	const QJsonArray arr = doc.array();
	for (const QJsonValue &v : arr) {
		const QJsonObject o = v.toObject();
		HfrCue c;
		c.type = o.value(QStringLiteral("type")).toInt(0);
		c.waitMs = o.value(QStringLiteral("waitMs")).toInt(0);
		c.canvas = o.value(QStringLiteral("canvas")).toString().trimmed();
		c.scene = o.value(QStringLiteral("scene")).toString().trimmed();
		if (c.type == 6) {
			if (!c.canvas.isEmpty()) {
				imported.push_back(c);
			}
		} else if (c.type >= 1 && c.type <= 5) {
			imported.push_back(c);
		} else if (!c.canvas.isEmpty() && !c.scene.isEmpty()) {
			imported.push_back(c);
		}
	}
	if (imported.isEmpty()) {
		SetStatus(QStringLiteral("导入失败：文件里没有有效的画布/场景条目（共 %1 条记录）").arg(arr.size()), 1);
		return;
	}
	const int skipped = arr.size() - imported.size();
	QString ask;
	if (skipped > 0) {
		ask = QStringLiteral("将用文件中的 %1 条节目单替换当前 %2 条（另有 %3 条无效记录被忽略），是否继续？")
			      .arg(imported.size())
			      .arg(cues.size())
			      .arg(skipped);
	} else {
		ask = QStringLiteral("将用文件中的 %1 条节目单替换当前 %2 条，是否继续？")
			      .arg(imported.size())
			      .arg(cues.size());
	}
	if (QMessageBox::question(this, QStringLiteral("导入节目单"), ask) != QMessageBox::Yes) {
		return;
	}
	if (cueAutoPlaying) {
		cueTimer->stop();
		cueAutoPlaying = false;
		btnCueAuto->setText(QStringLiteral("▶ 自动播放"));
	}
	cues = imported;
	cueIndex = 0;
	RefreshCueList();
	SaveState();
	SetStatus(QStringLiteral("已导入节目单 %1 条 → %2").arg(cues.size()).arg(QFileInfo(path).fileName()));
	HfrLog("rundown imported");
}

void HFRConsoleDock::AddCurrentSceneToRundown()
{
	obs_canvas_t *cv = CanvasFromRow(canvasList->currentRow());
	QListWidgetItem *item = sceneList->currentItem();
	if (!cv || !item) {
		SetStatus(QStringLiteral("请先选中画布与其场景，再加入节目单"), 1);
		return;
	}
	obs_source_t *scSrc = reinterpret_cast<obs_source_t *>(
		static_cast<quintptr>(item->data(Qt::UserRole).toULongLong()));
	if (!scSrc) {
		return;
	}
	HfrCue cue;
	cue.canvas = QString::fromUtf8(obs_canvas_get_name(cv));
	cue.scene = QString::fromUtf8(obs_source_get_name(scSrc));
	cues.push_back(cue);
	RefreshCueList();
	SaveState();
	status->setText(QStringLiteral("已加入节目单（共 %1 条）：%2 → %3").arg(cues.size()).arg(cue.canvas, cue.scene));
}

void HFRConsoleDock::RemoveSelectedCue()
{
	const int row = cueList ? cueList->currentRow() : -1;
	if (row < 0 || row >= cues.size()) {
		SetStatus(QStringLiteral("请先选中节目单条目"), 1);
		return;
	}
	cues.removeAt(row);
	if (cueIndex >= cues.size()) {
		cueIndex = cues.size() - 1;
	}
	if (cueIndex > row) {
		cueIndex--;
	}
	RefreshCueList();
	SaveState();
	status->setText(QStringLiteral("已删除节目单条目（剩 %1 条）").arg(cues.size()));
}

void HFRConsoleDock::MoveCue(int delta)
{
	const int row = cueList ? cueList->currentRow() : -1;
	const int to = row + delta;
	if (row < 0 || row >= cues.size() || to < 0 || to >= cues.size()) {
		return;
	}
	cues.swapItemsAt(row, to);
	if (cueIndex == row) {
		cueIndex = to;
	} else if (cueIndex == to) {
		cueIndex = row;
	}
	RefreshCueList();
	cueList->setCurrentRow(to);
	SaveState();
}

void HFRConsoleDock::ExecuteCue(int index)
{
	if (index < 0 || index >= cues.size()) {
		SetStatus(QStringLiteral("请先选择节目单条目"), 1);
		return;
	}
	const HfrCue cue = cues.at(index);

	/* 特殊条目：幻灯片翻页 / 黑场 / 恢复画面 */
	if (cue.type == 1 || cue.type == 2) {
		obs_source_t *slides = HfrFindFirstSlidesSource();
		if (!slides) {
			SetStatus(QStringLiteral("节目单：没有幻灯片来源（先加「幻灯片（自研引擎）」）"), 2);
			return;
		}
		bool moved = false;
		if (cue.type == 1) {
			moved = HfrSlidesNextAnim(slides);
			if (!moved) {
				const int cur = HfrSlidesPage(slides);
				if (cur + 1 < HfrSlidesPageCount(slides)) {
					HfrSlidesSetPage(slides, cur + 1);
					moved = true;
				}
			}
		} else {
			moved = HfrSlidesPrevAnim(slides);
			if (!moved) {
				const int cur = HfrSlidesPage(slides);
				if (cur > 0) {
					HfrSlidesSetPage(slides, cur - 1);
					moved = true;
				}
			}
		}
		cueIndex = index;
		RefreshCueList();
		SaveState();
		status->setText(QStringLiteral("节目单 %1/%2：幻灯片%3")
					.arg(index + 1)
					.arg(cues.size())
					.arg(cue.type == 1 ? QStringLiteral("下一页")
							  : QStringLiteral("上一页")));
		return;
	}
	if (cue.type == 5) {
		cueIndex = index;
		RefreshCueList();
		SaveState();
		const int ms = std::max(100, cue.waitMs);
		if (cueAutoPlaying) {
			status->setText(QStringLiteral("节目单 %1/%2：延时 %3 秒（自动继续）")
						.arg(index + 1)
						.arg(cues.size())
						.arg(ms / 1000.0, 0, 'f', 1));
			QTimer::singleShot(ms, this, [this, index]() {
				if (cueAutoPlaying && cueIndex == index) {
					NextCue();
				}
			});
		} else {
			status->setText(QStringLiteral("节目单 %1/%2：延时 %3 秒（手动模式：按下一步继续）")
						.arg(index + 1)
						.arg(cues.size())
						.arg(ms / 1000.0, 0, 'f', 1));
		}
		return;
	}
	if (cue.type == 6) {
		int targetRow = -1;
		for (int r = 0; r < (canvasList ? canvasList->count() : 0); r++) {
			obs_canvas_t *c = CanvasFromRow(r);
			if (c && QString::fromUtf8(obs_canvas_get_name(c)) == cue.canvas) {
				targetRow = r;
				break;
			}
		}
		if (targetRow < 0) {
			SetStatus(QStringLiteral("节目单：切换操作画布失败，找不到「%1」").arg(cue.canvas), 2);
			return;
		}
		canvasList->setCurrentRow(targetRow);
		cueIndex = index;
		RefreshCueList();
		SaveState();
		status->setText(QStringLiteral("节目单 %1/%2：操作画布 → %3")
					.arg(index + 1)
					.arg(cues.size())
					.arg(cue.canvas));
		return;
	}
	if (cue.type == 3 || cue.type == 4) {
		const bool wantOn = (cue.type == 3);
		if (blackoutActive != wantOn) {
			ToggleBlackout();
		}
		cueIndex = index;
		RefreshCueList();
		SaveState();
		status->setText(QStringLiteral("节目单 %1/%2：%3")
					.arg(index + 1)
					.arg(cues.size())
					.arg(wantOn ? QStringLiteral("黑场") : QStringLiteral("恢复画面")));
		return;
	}

	bool isMain = false;
	obs_canvas_t *cv = FindCanvasByNameForCue(canvases, cue.canvas, &isMain);
	if (!cv) {
		SetStatus(QStringLiteral("节目单：找不到画布「%1」").arg(cue.canvas), 2);
		return;
	}
	obs_source_t *scSrc = FindSceneSourceByNameForCue(cv, cue.scene);
	if (!scSrc) {
		if (isMain) {
			obs_canvas_release(cv);
		}
		SetStatus(QStringLiteral("节目单：找不到场景「%1」").arg(cue.scene), 2);
		return;
	}
	obs_canvas_set_channel(cv, 0, scSrc);
	if (CanvasFromRow(canvasList->currentRow()) == cv) {
		UpdateSceneListForSelection();
	}
	if (isMain) {
		obs_canvas_release(cv);
	}
	cueIndex = index;
	RefreshCueList();
	SaveState();
	status->setText(QStringLiteral("节目单 %1/%2：%3 → %4")
				.arg(index + 1)
				.arg(cues.size())
				.arg(cue.canvas, cue.scene));
}

void HFRConsoleDock::NextCue()
{
	if (cues.isEmpty()) {
		SetStatus(QStringLiteral("节目单为空：先加入当前场景"), 1);
		return;
	}
	if (cueIndex + 1 >= cues.size()) {
		status->setText(QStringLiteral("已是最后一条（%1 条）").arg(cues.size()));
		return;
	}
	ExecuteCue(cueIndex + 1);
}

void HFRConsoleDock::PrevCue()
{
	if (cues.isEmpty()) {
		return;
	}
	ExecuteCue(cueIndex <= 0 ? 0 : cueIndex - 1);
}

void HFRConsoleDock::ToggleAutoPlay()
{
	if (!cueTimer || !btnCueAuto) {
		return;
	}
	if (cueAutoPlaying) {
		cueTimer->stop();
		cueAutoPlaying = false;
		btnCueAuto->setText(QStringLiteral("▶自动播放"));
		status->setText(QStringLiteral("自动播放已暂停（第 %1/%2 条）").arg(cueIndex + 1).arg(cues.size()));
		return;
	}
	if (cues.isEmpty()) {
		SetStatus(QStringLiteral("节目单为空：先加入当前场景"), 1);
		return;
	}
	cueAutoPlaying = true;
	btnCueAuto->setText(QStringLiteral("■停止自动"));
	cueTimer->start(qMax(1, cueInterval ? cueInterval->value() : 5) * 1000);
	status->setText(QStringLiteral("自动播放中：每 %1 秒推进一条（共 %2 条）")
				.arg(qMax(1, cueInterval ? cueInterval->value() : 5))
				.arg(cues.size()));
}

/* ---------------- 素材池 / 倒计时 ---------------- */

void HFRConsoleDock::RefreshMediaList()
{
	if (!mediaList) {
		return;
	}
	mediaList->clear();
	for (const HfrMedia &m : mediaFiles) {
		QListWidgetItem *item = new QListWidgetItem(
			QStringLiteral("%1（%2）→ 画布「%3」/ 场景「%4」").arg(m.file, m.type, m.canvas, m.scene),
			mediaList);
		item->setToolTip(QStringLiteral("源：%1（双击定位到该画布/场景）").arg(m.source));
	}
}

void HFRConsoleDock::ImportMediaIntoScene()
{
	obs_canvas_t *cv = CanvasFromRow(canvasList->currentRow());
	QListWidgetItem *item = sceneList->currentItem();
	if (!cv || !item) {
		SetStatus(QStringLiteral("请先选中画布与其场景，再导入素材"), 1);
		return;
	}
	obs_source_t *scSrc = reinterpret_cast<obs_source_t *>(
		static_cast<quintptr>(item->data(Qt::UserRole).toULongLong()));
	obs_scene_t *sc = scSrc ? obs_scene_from_source(scSrc) : nullptr;
	if (!sc) {
		return;
	}
	const QString file = QFileDialog::getOpenFileName(
		this, QStringLiteral("选择素材（图片/视频/音频）"), QString(),
		QStringLiteral("媒体文件 (*.mp4 *.mov *.mkv *.avi *.webm *.png *.jpg *.jpeg *.bmp *.gif "
			       "*.mp3 *.wav *.flac *.aac *.m4a);;所有文件 (*)"));
	if (file.isEmpty()) {
		return;
	}
	ImportMediaFile(file);
}

/* 供拖拽复用：把单个媒体文件加入当前画布的场景 */
void HFRConsoleDock::ImportMediaFile(const QString &file)
{
	obs_canvas_t *cv = CanvasFromRow(canvasList->currentRow());
	QListWidgetItem *item = sceneList->currentItem();
	if (!cv || !item) {
		SetStatus(QStringLiteral("请先选中画布与其场景，再导入素材"), 1);
		return;
	}
	obs_source_t *scSrc = reinterpret_cast<obs_source_t *>(
		static_cast<quintptr>(item->data(Qt::UserRole).toULongLong()));
	obs_scene_t *sc = scSrc ? obs_scene_from_source(scSrc) : nullptr;
	if (!sc) {
		return;
	}
	const QString ext = QFileInfo(file).suffix().toLower();
	const bool isImage = (ext == QStringLiteral("png") || ext == QStringLiteral("jpg") ||
			      ext == QStringLiteral("jpeg") || ext == QStringLiteral("bmp") ||
			      ext == QStringLiteral("gif"));
	const QString nativeFile = QDir::toNativeSeparators(file);
	obs_data_t *s = obs_data_create();
	obs_source_t *src = nullptr;
	const QString name = QStringLiteral("素材 · ") + QFileInfo(file).fileName();
	if (isImage) {
		obs_data_set_string(s, "file", nativeFile.toUtf8().constData());
		src = obs_source_create("image_source", name.toUtf8().constData(), s, nullptr);
	} else {
		obs_data_set_bool(s, "is_local_file", true);
		obs_data_set_string(s, "local_file", nativeFile.toUtf8().constData());
		obs_data_set_bool(s, "looping", true);
		obs_data_set_bool(s, "restart_on_activate", true);
		src = obs_source_create("ffmpeg_source", name.toUtf8().constData(), s, nullptr);
	}
	obs_data_release(s);
	if (!src) {
		SetStatus(QStringLiteral("创建素材源失败（插件缺失或格式不支持）"), 2);
		return;
	}
	obs_sceneitem_t *sit = obs_scene_add(sc, src);
	if (sit) {
		struct obs_video_info ovi = {};
		uint32_t w = 1280, h = 720;
		if (obs_canvas_get_video_info(cv, &ovi) && ovi.base_width && ovi.base_height) {
			w = ovi.base_width;
			h = ovi.base_height;
		}
		obs_sceneitem_set_bounds_type(sit, OBS_BOUNDS_SCALE_INNER);
		struct vec2 sz = {(float)w, (float)h};
		obs_sceneitem_set_bounds(sit, &sz);
		struct vec2 pos = {(float)w / 2.0f, (float)h / 2.0f};
		obs_sceneitem_set_pos(sit, &pos);
		obs_sceneitem_set_alignment(sit, OBS_ALIGN_CENTER);
	}
	const QString srcName = QString::fromUtf8(obs_source_get_name(src));
	obs_source_release(src);
	HfrMedia media;
	media.file = QFileInfo(file).fileName();
	media.type = isImage ? QStringLiteral("图片") : QStringLiteral("音视频");
	media.canvas = QString::fromUtf8(obs_canvas_get_name(cv));
	media.scene = QString::fromUtf8(obs_source_get_name(scSrc));
	media.source = srcName;
	mediaFiles.append(media);
	RefreshMediaList();
	status->setText(QStringLiteral("已导入素材：%1（%2）")
				.arg(QFileInfo(file).fileName(), isImage ? QStringLiteral("图片") : QStringLiteral("音视频，循环")));
	HfrLog("media imported into scene");
}

/* 拖拽素材文件到导播坞 → 导入当前场景 */
void HFRConsoleDock::dragEnterEvent(QDragEnterEvent *event)
{
	if (event && event->mimeData()->hasUrls()) {
		event->acceptProposedAction();
	}
}

void HFRConsoleDock::dropEvent(QDropEvent *event)
{
	if (!event) {
		return;
	}
	int n = 0;
	for (const QUrl &u : event->mimeData()->urls()) {
		if (!u.isLocalFile()) {
			continue;
		}
		ImportMediaFile(u.toLocalFile());
		n++;
	}
	if (n > 0) {
		status->setText(QStringLiteral("已拖入 %1 个素材到当前场景").arg(n));
	}
	event->acceptProposedAction();
}

/* 素材列表双击：把该素材所在的画布+场景切为当前，并提示源名（不选中源） */
void HFRConsoleDock::LocateMediaAt(int row)
{
	if (row < 0 || row >= mediaFiles.size()) {
		return;
	}
	const HfrMedia &m = mediaFiles.at(row);
	bool isMain = false;
	obs_canvas_t *cv = FindCanvasByNameForCue(canvases, m.canvas, &isMain);
	if (!cv) {
		SetStatus(QStringLiteral("素材定位：找不到画布「%1」").arg(m.canvas), 2);
		return;
	}
	obs_source_t *scSrc = FindSceneSourceByNameForCue(cv, m.scene);
	if (!scSrc) {
		if (isMain) {
			obs_canvas_release(cv);
		}
		SetStatus(QStringLiteral("素材定位：找不到场景「%1」").arg(m.scene), 2);
		return;
	}
	obs_canvas_set_channel(cv, 0, scSrc);
	for (int i = 0; i < canvasList->count(); i++) {
		if (CanvasFromRow(i) == cv) {
			canvasList->setCurrentRow(i);
			break;
		}
	}
	if (isMain) {
		obs_canvas_release(cv);
	}
	UpdateSceneListForSelection();
	SetStatus(QStringLiteral("已定位：画布「%1」/ 场景「%2」，源「%3」").arg(m.canvas, m.scene, m.source));
	HfrLog("media located by double click");
}

void HFRConsoleDock::AddCountdownOverlay()
{
	obs_canvas_t *cv = CanvasFromRow(canvasList->currentRow());
	QListWidgetItem *item = sceneList->currentItem();
	if (!cv || !item) {
		SetStatus(QStringLiteral("请先选中画布与其场景，再加倒计时"), 1);
		return;
	}
	obs_source_t *scSrc = reinterpret_cast<obs_source_t *>(
		static_cast<quintptr>(item->data(Qt::UserRole).toULongLong()));
	obs_scene_t *sc = scSrc ? obs_scene_from_source(scSrc) : nullptr;
	if (!sc) {
		return;
	}
	bool ok = false;
	const int minutes = QInputDialog::getInt(this, QStringLiteral("加倒计时"),
						 QStringLiteral("倒计时时长（分钟，1–600）"), 5, 1, 600, 1, &ok);
	if (!ok) {
		return;
	}
	const qint64 durationMs = (qint64)minutes * 60 * 1000;
	const QString countdownTitle = QStringLiteral("倒计时 · %1分钟").arg(minutes);
	const QString initialText = QStringLiteral("%1:00").arg(minutes, 2, 10, QLatin1Char('0'));

	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "text", initialText.toUtf8().constData());
	obs_data_set_string(s, "font", "Microsoft YaHei");
	obs_data_set_int(s, "font_size", 96);
	obs_data_set_int(s, "color1", 0xFFFFFFFF);
	obs_data_set_int(s, "color2", 0xFF000000);
	obs_data_set_bool(s, "outline", true);
	obs_data_set_int(s, "outline_color", 0xFF000000);
	obs_data_set_int(s, "outline_size", 5);
	obs_source_t *src = obs_source_create("text_ft2_source_v2", countdownTitle.toUtf8().constData(), s, nullptr);
	if (!src) {
		src = obs_source_create("text_gdiplus_v2", countdownTitle.toUtf8().constData(), s, nullptr);
	}
	obs_data_release(s);
	if (!src) {
		SetStatus(QStringLiteral("创建文本源失败（obs-text 插件未加载）"), 2);
		return;
	}
	obs_sceneitem_t *sit = obs_scene_add(sc, src);
	if (sit) {
		obs_sceneitem_set_alignment(sit, OBS_ALIGN_CENTER | OBS_ALIGN_TOP);
		struct obs_video_info ovi = {};
		uint32_t w = 1280, h = 720;
		if (obs_canvas_get_video_info(cv, &ovi) && ovi.base_width && ovi.base_height) {
			w = ovi.base_width;
			h = ovi.base_height;
		}
		struct vec2 pos = {(float)w / 2.0f, (float)h * 0.08f};
		obs_sceneitem_set_pos(sit, &pos);
	}
	obs_source_t *hold = obs_source_get_ref(src);
	if (hold) {
		countdowns.insert(hold, QDateTime::currentMSecsSinceEpoch() + durationMs);
	}
	obs_source_release(src);
	SetStatus(QStringLiteral("已添加 %1 分钟倒计时（顶部居中，每秒刷新）").arg(minutes));
	HfrLog("countdown overlay added");
}

/* ---------------- 编码器公共创建（绑画布 video / 全局音频） ---------------- */

static obs_encoder_t *CreateCanvasVideoEncoder(obs_canvas_t *cv, const QString &encId, QString *usedId)
{
	struct obs_video_info ovi = {};
	uint32_t w = 1280, h = 720;
	if (obs_canvas_get_video_info(cv, &ovi) && ovi.base_width && ovi.base_height) {
		w = ovi.base_width;
		h = ovi.base_height;
	}
	UNUSED_PARAMETER(w);
	UNUSED_PARAMETER(h);

	const QString canvasName = QString::fromUtf8(obs_canvas_get_name(cv));
	const QString nm = canvasName + QStringLiteral(" · 视频");

	auto make = [&](const QString &id) -> obs_encoder_t * {
		obs_data_t *vs = obs_data_create();
		obs_data_set_string(vs, "rate_control", "CBR");
		obs_data_set_int(vs, "bitrate", 6000);
		obs_data_set_int(vs, "keyint_sec", 2);
		if (id == QStringLiteral("obs_x264")) {
			obs_data_set_string(vs, "preset", "veryfast");
			obs_data_set_string(vs, "profile", "high");
		}
		obs_encoder_t *e = obs_video_encoder_create(id.toUtf8().constData(), nm.toUtf8().constData(), vs, nullptr);
		obs_data_release(vs);
		return e;
	};

	QString id = encId.isEmpty() ? QStringLiteral("obs_x264") : encId;
	obs_encoder_t *e = make(id);
	if (!e && id != QStringLiteral("obs_x264")) {
		id = QStringLiteral("obs_x264");
		e = make(id);
	}
	if (e && usedId) {
		*usedId = id;
	}
	return e;
}

static obs_encoder_t *CreateCanvasAudioEncoder(obs_canvas_t *cv, size_t mixerIdx = 0)
{
	const QString canvasName = QString::fromUtf8(obs_canvas_get_name(cv));
	const QString nm = canvasName + QStringLiteral(" · 音轨%1").arg((int)mixerIdx + 1);
	obs_data_t *as = obs_data_create();
	obs_data_set_int(as, "bitrate", 160);
	obs_encoder_t *e = obs_audio_encoder_create("ffmpeg_aac", nm.toUtf8().constData(), as, mixerIdx, nullptr);
	obs_data_release(as);
	if (e) {
		obs_encoder_set_audio(e, obs_get_audio());
	}
	return e;
}

/* ---------------- 每画布独立录制（编码器绑定画布 video） ---------------- */

void HFRConsoleDock::StartRecordingSelected()
{
	obs_canvas_t *cv = CanvasFromRow(canvasList->currentRow());
	if (!cv) {
		SetStatus(QStringLiteral("请先点选画布"), 1);
		return;
	}
	if (cv == obs_get_main_canvas()) {
		SetStatus(QStringLiteral("主画布请用 OBS 自带录制按钮；这里录制用户画布"), 1);
		return;
	}
	if (recordings.contains(cv)) {
		SetStatus(QStringLiteral("该画布已在录制"), 1);
		return;
	}

	video_t *video = obs_canvas_get_video(cv);
	if (!video) {
		SetStatus(QStringLiteral("该画布没有视频输出"), 2);
		return;
	}

	char *profilePath = obs_frontend_get_current_profile_path();
	QString dir = profilePath ? QString::fromUtf8(profilePath) : QString();
	if (profilePath) {
		bfree(profilePath);
	}
	if (dir.isEmpty()) {
		SetStatus(QStringLiteral("取不到 profile 路径"), 2);
		return;
	}
	const QString canvasName = QString::fromUtf8(obs_canvas_get_name(cv));
	const QString ts = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
	const QString file = dir + QStringLiteral("/hfr_") + canvasName + QStringLiteral("_") + ts + QStringLiteral(".mkv");

	QString encId = encCombo ? encCombo->currentData().toString() : QStringLiteral("obs_x264");
	QString usedEnc;
	obs_encoder_t *venc = CreateCanvasVideoEncoder(cv, encId, &usedEnc);
	if (!venc) {
		SetStatus(QStringLiteral("创建视频编码器失败（无可用 H.264 编码器）"), 2);
		return;
	}
	encId = usedEnc;
	obs_encoder_set_video(venc, video);

	/* 音频路由矩阵：统计所有音频源使用的音轨位（mixer），按 0..最大轨连续创建编码器 */
	uint32_t usedMask = 0;
	auto enumAudioMask = [](void *param, obs_source_t *s) -> bool {
		auto *mask = static_cast<uint32_t *>(param);
		if (s && (obs_source_get_output_flags(s) & OBS_SOURCE_AUDIO)) {
			*mask |= obs_source_get_audio_mixers(s);
		}
		return true;
	};
	obs_enum_sources(enumAudioMask, &usedMask);
	if (chkDualTrack && chkDualTrack->isChecked() && !commentatorPrev.isEmpty()) {
		usedMask |= (1u << 1); /* 解说源在第 2 轨 */
	}
	if (usedMask == 0) {
		usedMask = 1; /* 默认只有现场轨 */
	}
	int maxTrack = 0;
	for (int b = 0; b < 6; b++) {
		if (usedMask & (1u << b)) {
			maxTrack = b;
		}
	}
	QVector<obs_encoder_t *> aencs;
	for (int b = 0; b <= maxTrack; b++) {
		obs_encoder_t *e = CreateCanvasAudioEncoder(cv, (size_t)b);
		if (e) {
			aencs.push_back(e);
		}
	}

	obs_data_t *os = obs_data_create();
	obs_data_set_string(os, "path", file.toUtf8().constData());
	obs_data_t *fmt = obs_data_create();
	obs_data_set_string(fmt, "extension", "mkv");
	obs_data_set_obj(os, "format", fmt);
	obs_data_release(fmt);

	QString oname = canvasName + QStringLiteral(" · 录制");
	obs_output_t *out = obs_output_create("ffmpeg_muxer", oname.toUtf8().constData(), os, nullptr);
	obs_data_release(os);
	if (!out) {
		obs_encoder_release(venc);
		for (obs_encoder_t *e : aencs) {
			obs_encoder_release(e);
		}
		SetStatus(QStringLiteral("创建输出失败（ffmpeg_muxer 不可用？）"), 2);
		return;
	}
	obs_output_set_media(out, video, aencs.isEmpty() ? nullptr : obs_get_audio());
	obs_output_set_video_encoder(out, venc);
	for (int b = 0; b < aencs.size(); b++) {
		obs_output_set_audio_encoder(out, aencs[b], (size_t)b);
	}

	if (!obs_output_start(out)) {
		const char *err = obs_output_get_last_error(out);
		SetStatus(QStringLiteral("开始录制失败：%1").arg(err ? QString::fromUtf8(err) : QStringLiteral("未知")), 2);
		obs_output_release(out);
		obs_encoder_release(venc);
		for (obs_encoder_t *e : aencs) {
			obs_encoder_release(e);
		}
		return;
	}

	HfrRecording rec;
	rec.out = out;
	rec.venc = venc;
	rec.aencs = aencs;
	rec.path = file;
	recordings.insert(cv, rec);
	status->setText(QStringLiteral("录制中：%1 → %2（编码器 %3）").arg(canvasName).arg(QFileInfo(file).fileName()).arg(encId));
	UpdateRecordingIndicators();
	HfrLog("canvas recording started");
}

void HFRConsoleDock::UpdateRecordingIndicators()
{
	for (int i = 0; i < canvasList->count(); i++) {
		QListWidgetItem *item = canvasList->item(i);
		if (!item) {
			continue;
		}
		HFRCanvasRow *row = static_cast<HFRCanvasRow *>(canvasList->itemWidget(item));
		obs_canvas_t *cv = CanvasFromRow(i);
		if (row && cv) {
			row->SetRecording(recordings.contains(cv));
		}
	}
}

void HFRConsoleDock::StopRecordingSelected()
{
	obs_canvas_t *cv = CanvasFromRow(canvasList->currentRow());
	if (!cv || !recordings.contains(cv)) {
		SetStatus(QStringLiteral("该画布没有在录制"), 1);
		return;
	}
	HfrRecording rec = recordings.take(cv);
	const QString name = QFileInfo(rec.path).fileName();
	if (rec.out) {
		obs_output_stop(rec.out);
		obs_output_release(rec.out);
	}
	if (rec.venc) {
		obs_encoder_release(rec.venc);
	}
	for (obs_encoder_t *e : rec.aencs) {
		obs_encoder_release(e);
	}
	status->setText(QStringLiteral("已停止录制：%1").arg(name));
	UpdateRecordingIndicators();
	HfrLog("canvas recording stopped");
}

void HFRConsoleDock::StopAllRecordings()
{
	const auto keys = recordings.keys();
	for (obs_canvas_t *cv : keys) {
		if (recordings.contains(cv)) {
			HfrRecording rec = recordings.take(cv);
			if (rec.out) {
				obs_output_stop(rec.out);
				obs_output_release(rec.out);
			}
			if (rec.venc) {
				obs_encoder_release(rec.venc);
			}
			for (obs_encoder_t *e : rec.aencs) {
				obs_encoder_release(e);
			}
		}
	}
	UpdateRecordingIndicators();
}

/* ---------------- 音频双轨：解说源（只进第 2 轨） ---------------- */

void HFRConsoleDock::OpenAudioRouting()
{
	HFRAudioRoutingDialog dlg(this);
	if (dlg.exec() == QDialog::Accepted) {
		SetStatus(QStringLiteral("音频路由已应用：录制时按被使用的音轨创建多轨（0..最大轨连续）"));
	}
}

void HFRConsoleDock::RefreshCommentatorCombo()
{
	if (!commentatorCombo) {
		return;
	}
	const QString keep = commentatorCombo->currentData().toString();
	commentatorCombo->clear();
	auto enumCb = [](void *param, obs_source_t *src) -> bool {
		auto *combo = static_cast<QComboBox *>(param);
		if (src && (obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO)) {
			const QString n = QString::fromUtf8(obs_source_get_name(src));
			if (!n.isEmpty() && combo->findData(n) < 0) {
				combo->addItem(n, n);
			}
		}
		return true;
	};
	obs_enum_sources(enumCb, commentatorCombo);
	const int idx = commentatorCombo->findData(keep);
	if (idx >= 0) {
		commentatorCombo->setCurrentIndex(idx);
	}
}

void HFRConsoleDock::ApplyCommentatorSource()
{
	if (!commentatorCombo) {
		return;
	}
	const QString name = commentatorCombo->currentData().toString();
	if (name.isEmpty()) {
		SetStatus(QStringLiteral("请先选择解说音源（音频源）"), 1);
		return;
	}
	obs_source_t *src = obs_get_source_by_name(name.toUtf8().constData());
	if (!src) {
		SetStatus(QStringLiteral("找不到音源「%1」").arg(name), 2);
		return;
	}
	RestoreCommentatorMixers();
	commentatorPrev.insert(src, obs_source_get_audio_mixers(src));
	/* 第 2 轨 = mixers 位 1：该源只录进第 2 轨，不进第 1 轨（现场混音不受影响） */
	obs_source_set_audio_mixers(src, 1u << 1);
	SetStatus(QStringLiteral("解说源「%1」→ 只录第 2 音轨（勾选「双轨」后录制生效）").arg(name));
}

void HFRConsoleDock::RestoreCommentatorMixers()
{
	for (auto it = commentatorPrev.begin(); it != commentatorPrev.end(); ++it) {
		if (it.key()) {
			obs_source_set_audio_mixers(it.key(), it.value());
			obs_source_release(it.key());
		}
	}
	commentatorPrev.clear();
}

/* ---------------- PPT（本地 LibreOffice 转 PDF + pdfium 渲染，无桥接） ---------------- */

static HFRPptDocument &PptSelfTestDoc()
{
	static HFRPptDocument doc;
	return doc;
}

static void WriteBmp32File(const QString &path, uint32_t w, uint32_t h, const std::vector<uint8_t> &bgra)
{
	const uint32_t imgSize = w * 4 * h;
	const uint32_t fileSize = 54 + imgSize;
	QFile f(path);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		return;
	}
	uint8_t hdr[54] = {0};
	hdr[0] = 'B';
	hdr[1] = 'M';
	memcpy(hdr + 2, &fileSize, 4);
	uint32_t off = 54;
	memcpy(hdr + 10, &off, 4);
	uint32_t hdrSize = 40;
	memcpy(hdr + 14, &hdrSize, 4);
	memcpy(hdr + 18, &w, 4);
	memcpy(hdr + 22, &h, 4);
	uint16_t planes = 1, bpp = 32;
	memcpy(hdr + 26, &planes, 2);
	memcpy(hdr + 28, &bpp, 2);
	memcpy(hdr + 34, &imgSize, 4);
	f.write((const char *)hdr, 54);
	f.write((const char *)bgra.data(), (qint64)imgSize);
	f.close();
}

static void PptRenderAndReport(QLabel *status)
{
	HFRPptDocument &doc = PptSelfTestDoc();
	if (!doc.IsOpen()) {
		status->setText(QStringLiteral("PPT：尚未打开文件"));
		ApplyStatusLevel(status, 1);
		return;
	}
	std::vector<uint8_t> buf;
	QString err;
	if (!doc.RenderCurrentPart(1280, 720, buf, &err)) {
		status->setText(QStringLiteral("PPT 渲染失败：%1").arg(err));
		ApplyStatusLevel(status, 2);
		return;
	}
	const QString out = HfrPptTestOutputDir() + QStringLiteral("/hfr_ppt_preview.bmp");
	WriteBmp32File(out, 1280, 720, buf);

	size_t nonWhite = 0;
	const size_t px = (size_t)1280 * 720;
	for (size_t i = 0; i < px; i++) {
		const uint8_t b = buf[i * 4 + 0], g = buf[i * 4 + 1], r = buf[i * 4 + 2];
		if (!(b > 245 && g > 245 && r > 245)) {
			nonWhite++;
		}
	}
	const double pct = px ? (100.0 * (double)nonWhite / (double)px) : 0.0;
	status->setText(QStringLiteral("PPT 渲染成功：非白像素 %1%（1280x720，第 %2/%3 页）→ %4")
				.arg(pct, 0, 'f', 2)
				.arg(doc.CurrentPart() + 1)
				.arg(doc.PartCount())
				.arg(out));
}

void HFRConsoleDock::PptOpenTest()
{
	const QString file = QFileDialog::getOpenFileName(this, QStringLiteral("选择 PPT 文件"), QString(),
							  QStringLiteral("演示文稿 (*.pptx *.ppt *.odp);;所有文件 (*)"));
	if (file.isEmpty()) {
		return;
	}
	QString err;
	if (!PptSelfTestDoc().Open(file, &err)) {
		SetStatus(QStringLiteral("PPT 打开失败：%1").arg(err), 2);
		return;
	}
	PptRenderAndReport(status);
}

void HFRConsoleDock::PptNext()
{
	HFRPptDocument &doc = PptSelfTestDoc();
	if (!doc.IsOpen()) {
		SetStatus(QStringLiteral("PPT：尚未打开文件"), 1);
		return;
	}
	doc.NextPart();
	PptRenderAndReport(status);
}

void HFRConsoleDock::PptPrev()
{
	HFRPptDocument &doc = PptSelfTestDoc();
	if (!doc.IsOpen()) {
		SetStatus(QStringLiteral("PPT：尚未打开文件"), 1);
		return;
	}
	doc.PrevPart();
	PptRenderAndReport(status);
}

/* ---------------- 诊断信息导出（验收/远程排障） ---------------- */
static bool CollectSourceNamesCb(void *param, obs_source_t *src)
{
	auto *lines = static_cast<QStringList *>(param);
	if (!src) {
		return true;
	}
	const char *name = obs_source_get_name(src);
	const char *id = obs_source_get_id(src);
	lines->push_back(QStringLiteral("%1  [%2]")
			  .arg(name ? QString::fromUtf8(name) : QStringLiteral("?"))
			  .arg(id ? QString::fromUtf8(id) : QStringLiteral("?")));
	return true;
}

void HFRConsoleDock::ExportDiagnostics()
{
	char *prof = obs_frontend_get_current_profile_path();
	const QString profile = prof ? QString::fromUtf8(prof) : QString();
	if (prof) {
		bfree(prof);
	}
	QString logsDir;
	if (!profile.isEmpty()) {
		QDir up(profile);
		up.cdUp(); /* profiles */
		up.cdUp(); /* basic */
		up.cdUp(); /* obs-studio */
		logsDir = up.absolutePath() + QStringLiteral("/logs");
	}
	if (logsDir.isEmpty() || !QDir(logsDir).exists()) {
		logsDir = QDir::tempPath();
	}
	const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
	const QString outPath = QDir(logsDir).filePath(QStringLiteral("hfr_diag-%1.txt").arg(stamp));
	QFile f(outPath);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		SetStatus(QStringLiteral("诊断导出失败：无法写入 %1").arg(outPath), 2);
		return;
	}
	QTextStream ts(&f);
	ts << "HuafeiRongOBS diagnostics\n";
	ts << "time: " << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n";
	ts << "obs version: " << obs_get_version_string() << "\n";
	ts << "qt: " << qVersion() << "\n";
	ts << "os: " << QSysInfo::prettyProductName() << " / " << QSysInfo::currentCpuArchitecture()
	   << "\n";
	ts << "\n[screens]\n";
	const auto screens = QGuiApplication::screens();
	for (int i = 0; i < screens.size(); i++) {
		const QRect g = screens.at(i)->geometry();
		ts << i << ": " << screens.at(i)->name() << " " << g.width() << "x" << g.height()
		   << " @" << g.x() << "," << g.y() << " dpr=" << screens.at(i)->devicePixelRatio() << "\n";
	}
	ts << "\n[canvases]\n";
	for (obs_canvas_t *cv : canvases) {
		if (!cv) {
			continue;
		}
		struct obs_video_info ovi = {};
		obs_canvas_get_video_info(cv, &ovi);
		ts << "- " << QString::fromUtf8(obs_canvas_get_name(cv)) << " " << ovi.base_width << "x"
		   << ovi.base_height << " safe=" << (safeAreaPref.value(cv) ? 1 : 0) << "\n";
		for (const HfrWallTile &t : wallTiles.value(cv)) {
			ts << "    tile screen=" << t.screen << " " << t.w << "x" << t.h << " at " << t.x
			   << "," << t.y << " rot=" << t.rot << " blend=" << t.blendL << "/" << t.blendR
			   << "/" << t.blendT << "/" << t.blendB
			   << " key=" << t.kTLx << "," << t.kTLy << " " << t.kTRx << "," << t.kTRy << " "
			   << t.kBLx << "," << t.kBLy << " " << t.kBRx << "," << t.kBRy
			   << " curve=" << (t.HasCurve() ? 1 : 0) << " color=" << t.gammaV << "/" << t.liftV
			   << "/" << t.gainV << "\n";
		}
	}
	ts << "\n[projections]\n";
	for (const HFRProjection &pr : projections) {
		ts << "- canvas=" << (pr.canvas ? QString::fromUtf8(obs_canvas_get_name(pr.canvas))
						  : QStringLiteral("?"))
		   << " screen=" << pr.screenIndex << " tile=" << (pr.hasTile ? 1 : 0)
		   << " safe=" << ((pr.window && pr.window->HasSafeArea()) ? 1 : 0) << "\n";
	}
	ts << "\n[recordings]\n";
	for (auto it = recordings.begin(); it != recordings.end(); ++it) {
		ts << "- " << QFileInfo(it.value().path).fileName() << " audioTracks=" << it.value().aencs.size()
		   << "\n";
	}
	ts << "\n[rundown] count=" << cues.size() << " index=" << cueIndex << "\n";
	for (int i = 0; i < cues.size(); i++) {
		ts << i << ": type=" << cues[i].type << " canvas=" << cues[i].canvas
		   << " scene=" << cues[i].scene << " wait=" << cues[i].waitMs << "\n";
	}
	ts << "\n[sources]\n";
	QStringList srcLines;
	obs_enum_sources(CollectSourceNamesCb, &srcLines);
	for (const QString &l : srcLines) {
		ts << l << "\n";
	}
	/* 最新日志尾部 */
	const QDir ld(logsDir);
	const QStringList logs = ld.entryList({QStringLiteral("*.txt")}, QDir::Files, QDir::Time);
	if (!logs.isEmpty()) {
		QFile lf(ld.filePath(logs.first()));
		if (lf.open(QIODevice::ReadOnly)) {
			const QList<QByteArray> tail = lf.readAll().split('\n');
			const int from = std::max(0, (int)tail.size() - 120);
			ts << "\n[log tail] " << logs.first() << "\n";
			for (int i = from; i < tail.size(); i++) {
				ts << QString::fromUtf8(tail[i]).trimmed() << "\n";
			}
		}
	}
	ts.flush();
	f.close();
	SetStatus(QStringLiteral("诊断已导出：%1").arg(outPath));
}
/* ---------------- 持久化 ---------------- */

QString HFRConsoleDock::StateFilePath() const
{
	char *path = obs_frontend_get_current_profile_path();
	QString dir = path ? QString::fromUtf8(path) : QString();
	if (path) {
		bfree(path);
	}
	if (dir.isEmpty()) {
		return QString();
	}
	return dir + QStringLiteral("/hfr_canvases.json");
}

/* ---------------- 画布场景内容持久化（源 id/name/settings + 场景项变换/裁剪） ---------------- */
static bool CollectSceneItemsCb(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	Q_UNUSED(scene);
	auto *items = static_cast<std::vector<obs_sceneitem_t *> *>(param);
	if (item) {
		items->push_back(item);
	}
	return true;
}

static void CollectFiltersCb(obs_source_t *parent, obs_source_t *filter, void *param)
{
	Q_UNUSED(parent);
	auto *vec = static_cast<std::vector<obs_source_t *> *>(param);
	if (filter) {
		vec->push_back(filter);
	}
}

static void RestoreSceneItems(obs_scene_t *sc, const QJsonArray &items)
{
	if (!sc) {
		return;
	}
	for (const QJsonValue &v : items) {
		const QJsonObject io = v.toObject();
		const QString id = io.value(QStringLiteral("id")).toString();
		const QString name = io.value(QStringLiteral("name")).toString();
		if (id.isEmpty()) {
			continue;
		}
		obs_data_t *settings = nullptr;
		const QString sj = io.value(QStringLiteral("settings")).toString();
		if (!sj.isEmpty()) {
			settings = obs_data_create_from_json(sj.toUtf8().constData());
		}
		if (!settings) {
			settings = obs_data_create();
		}
		obs_source_t *src = obs_source_create(id.toUtf8().constData(), name.toUtf8().constData(), settings,
						       nullptr);
		obs_data_release(settings);
		if (!src) {
			continue;
		}
		obs_sceneitem_t *it = obs_scene_add(sc, src);
		if (it) {
			const QJsonObject to = io.value(QStringLiteral("transform")).toObject();
			struct obs_transform_info ti = {};
			ti.pos.x = (float)to.value(QStringLiteral("px")).toDouble(0.0);
			ti.pos.y = (float)to.value(QStringLiteral("py")).toDouble(0.0);
			ti.rot = (float)to.value(QStringLiteral("rot")).toDouble(0.0);
			ti.scale.x = (float)to.value(QStringLiteral("sx")).toDouble(1.0);
			ti.scale.y = (float)to.value(QStringLiteral("sy")).toDouble(1.0);
			ti.alignment = (uint32_t)to.value(QStringLiteral("align")).toInt(0);
			ti.bounds_type =
				(enum obs_bounds_type)to.value(QStringLiteral("bounds_type")).toInt(0);
			ti.bounds_alignment = (uint32_t)to.value(QStringLiteral("bounds_align")).toInt(0);
			ti.bounds.x = (float)to.value(QStringLiteral("bx")).toDouble(0.0);
			ti.bounds.y = (float)to.value(QStringLiteral("by")).toDouble(0.0);
			ti.crop_to_bounds = to.value(QStringLiteral("crop_to_bounds")).toBool(false);
			obs_sceneitem_set_info2(it, &ti);
			struct obs_sceneitem_crop crop = {};
			crop.left = to.value(QStringLiteral("crop_l")).toInt(0);
			crop.top = to.value(QStringLiteral("crop_t")).toInt(0);
			crop.right = to.value(QStringLiteral("crop_r")).toInt(0);
			crop.bottom = to.value(QStringLiteral("crop_b")).toInt(0);
			obs_sceneitem_set_crop(it, &crop);
			obs_sceneitem_set_visible(it, io.value(QStringLiteral("visible")).toBool(true));
			obs_source_set_audio_mixers(src,
						    (uint32_t)io.value(QStringLiteral("mixers")).toInt(1));
			obs_source_set_volume(src, (float)io.value(QStringLiteral("volume")).toDouble(1.0));
			obs_source_set_muted(src, io.value(QStringLiteral("muted")).toBool(false));
			/* 过滤器（按保存顺序重建） */
			for (const QJsonValue &fv : io.value(QStringLiteral("filters")).toArray()) {
				const QJsonObject fo = fv.toObject();
				const QString fid = fo.value(QStringLiteral("id")).toString();
				if (fid.isEmpty()) {
					continue;
				}
				obs_data_t *fs = nullptr;
				const QString fsj = fo.value(QStringLiteral("settings")).toString();
				if (!fsj.isEmpty()) {
					fs = obs_data_create_from_json(fsj.toUtf8().constData());
				}
				if (!fs) {
					fs = obs_data_create();
				}
				obs_source_t *filter = obs_source_create(
					fid.toUtf8().constData(),
					fo.value(QStringLiteral("name")).toString().toUtf8().constData(), fs,
					nullptr);
				obs_data_release(fs);
				if (!filter) {
					continue;
				}
				obs_source_filter_add(src, filter);
				obs_source_set_enabled(filter, fo.value(QStringLiteral("enabled")).toBool(true));
				obs_source_release(filter);
			}
		}
		obs_source_release(src);
	}
}

static QJsonObject SceneContentToJson(obs_source_t *sceneSource)
{
	QJsonObject so;
	so[QStringLiteral("name")] = QString::fromUtf8(obs_source_get_name(sceneSource));
	QJsonArray itemsArr;
	obs_scene_t *sc = obs_scene_from_source(sceneSource);
	if (sc) {
		std::vector<obs_sceneitem_t *> items;
		obs_scene_enum_items(sc, CollectSceneItemsCb, &items);
		for (obs_sceneitem_t *it : items) {
			obs_source_t *src = obs_sceneitem_get_source(it);
			if (!src) {
				continue;
			}
			const char *sid = obs_source_get_id(src);
			QJsonObject io;
			io[QStringLiteral("id")] = sid ? QString::fromUtf8(sid) : QString();
			io[QStringLiteral("name")] = QString::fromUtf8(obs_source_get_name(src));
			obs_data_t *st = obs_source_get_settings(src);
			if (st) {
				io[QStringLiteral("settings")] = QString::fromUtf8(obs_data_get_json(st));
				obs_data_release(st);
			}
			struct obs_transform_info ti = {};
			obs_sceneitem_get_info2(it, &ti);
			QJsonObject to;
			to[QStringLiteral("px")] = ti.pos.x;
			to[QStringLiteral("py")] = ti.pos.y;
			to[QStringLiteral("rot")] = ti.rot;
			to[QStringLiteral("sx")] = ti.scale.x;
			to[QStringLiteral("sy")] = ti.scale.y;
			to[QStringLiteral("align")] = (int)ti.alignment;
			to[QStringLiteral("bounds_type")] = (int)ti.bounds_type;
			to[QStringLiteral("bounds_align")] = (int)ti.bounds_alignment;
			to[QStringLiteral("bx")] = ti.bounds.x;
			to[QStringLiteral("by")] = ti.bounds.y;
			to[QStringLiteral("crop_to_bounds")] = ti.crop_to_bounds;
			struct obs_sceneitem_crop crop = {};
			obs_sceneitem_get_crop(it, &crop);
			to[QStringLiteral("crop_l")] = crop.left;
			to[QStringLiteral("crop_t")] = crop.top;
			to[QStringLiteral("crop_r")] = crop.right;
			to[QStringLiteral("crop_b")] = crop.bottom;
			io[QStringLiteral("transform")] = to;
			io[QStringLiteral("visible")] = obs_sceneitem_visible(it);
			io[QStringLiteral("mixers")] = (int)obs_source_get_audio_mixers(src);
			io[QStringLiteral("volume")] = (double)obs_source_get_volume(src);
			io[QStringLiteral("muted")] = obs_source_muted(src);
			QJsonArray filtersArr;
			std::vector<obs_source_t *> filters;
			obs_source_enum_filters(src, CollectFiltersCb, &filters);
			for (obs_source_t *f : filters) {
				const char *fid = obs_source_get_id(f);
				QJsonObject fo;
				fo[QStringLiteral("id")] = fid ? QString::fromUtf8(fid) : QString();
				fo[QStringLiteral("name")] = QString::fromUtf8(obs_source_get_name(f));
				fo[QStringLiteral("enabled")] = obs_source_enabled(f);
				obs_data_t *fs = obs_source_get_settings(f);
				if (fs) {
					fo[QStringLiteral("settings")] =
						QString::fromUtf8(obs_data_get_json(fs));
					obs_data_release(fs);
				}
				filtersArr.append(fo);
			}
			io[QStringLiteral("filters")] = filtersArr;
			itemsArr.append(io);
		}
	}
	so[QStringLiteral("items")] = itemsArr;
	return so;
}
void HFRConsoleDock::SaveState()
{
	if (loadingState) {
		return;
	}
	const QString file = StateFilePath();
	if (file.isEmpty()) {
		return;
	}

	QJsonArray canvasArr;
	for (obs_canvas_t *cv : canvases) {
		if (!cv) {
			continue;
		}
		struct obs_video_info ovi = {};
		obs_canvas_get_video_info(cv, &ovi);
		QJsonObject o;
		o[QStringLiteral("name")] = QString::fromUtf8(obs_canvas_get_name(cv));
		o[QStringLiteral("w")] = (int)ovi.base_width;
		o[QStringLiteral("h")] = (int)ovi.base_height;
		o[QStringLiteral("test")] = testSceneAdded.value(cv, false);
		o[QStringLiteral("safe")] = safeAreaPref.value(cv, false);

		QJsonArray tilesArr;
		for (const HfrWallTile &t : wallTiles.value(cv)) {
			QJsonObject to;
			to[QStringLiteral("screen")] = t.screen;
			to[QStringLiteral("x")] = t.x;
			to[QStringLiteral("y")] = t.y;
			to[QStringLiteral("w")] = t.w;
			to[QStringLiteral("h")] = t.h;
			to[QStringLiteral("rot")] = t.rot;
			to[QStringLiteral("blendL")] = t.blendL;
			to[QStringLiteral("blendR")] = t.blendR;
			to[QStringLiteral("blendT")] = t.blendT;
			to[QStringLiteral("blendB")] = t.blendB;
			to[QStringLiteral("kTLx")] = t.kTLx;
			to[QStringLiteral("kTLy")] = t.kTLy;
			to[QStringLiteral("kTRx")] = t.kTRx;
			to[QStringLiteral("kTRy")] = t.kTRy;
			to[QStringLiteral("kBLx")] = t.kBLx;
			to[QStringLiteral("kBLy")] = t.kBLy;
			to[QStringLiteral("kBRx")] = t.kBRx;
			to[QStringLiteral("kBRy")] = t.kBRy;
			to[QStringLiteral("gammaV")] = t.gammaV;
			to[QStringLiteral("liftV")] = t.liftV;
			to[QStringLiteral("gainV")] = t.gainV;
			to[QStringLiteral("cMTx")] = t.cMTx;
			to[QStringLiteral("cMTy")] = t.cMTy;
			to[QStringLiteral("cMRx")] = t.cMRx;
			to[QStringLiteral("cMRy")] = t.cMRy;
			to[QStringLiteral("cMBx")] = t.cMBx;
			to[QStringLiteral("cMBy")] = t.cMBy;
			to[QStringLiteral("cMLx")] = t.cMLx;
			to[QStringLiteral("cMLy")] = t.cMLy;
			to[QStringLiteral("cCx")] = t.cCx;
			to[QStringLiteral("cCy")] = t.cCy;
			tilesArr.append(to);
		}
		o[QStringLiteral("tiles")] = tilesArr;

		QJsonArray scenesArr;
		std::vector<obs_source_t *> scenes;
		obs_canvas_enum_scenes(cv, CollectSceneSourcesCallback, &scenes);
		for (obs_source_t *s : scenes) {
			scenesArr.append(SceneContentToJson(s));
		}
		o[QStringLiteral("scenes")] = scenesArr;

		obs_source_t *cur = obs_canvas_get_channel(cv, 0);
		/* 黑场中保存"黑场前"的场景，重启后回到正常画面而不是停在黑场 */
		if (blackoutActive && blackoutPrev.contains(cv) && blackoutPrev.value(cv)) {
			cur = blackoutPrev.value(cv);
		}
		o[QStringLiteral("cur")] = cur ? QString::fromUtf8(obs_source_get_name(cur)) : QString();
		canvasArr.append(o);
	}

	QJsonArray projArr;
	for (const HFRProjection &pr : projections) {
		if (!pr.window || !pr.canvas) {
			continue;
		}
		const auto screens = QGuiApplication::screens();
		QString screenName = (pr.screenIndex >= 0 && pr.screenIndex < screens.size())
					     ? screens.at(pr.screenIndex)->name()
					     : QString();
		QJsonObject o;
		o[QStringLiteral("canvas")] = QString::fromUtf8(obs_canvas_get_name(pr.canvas));
		o[QStringLiteral("screen")] = pr.screenIndex;
		o[QStringLiteral("screenName")] = screenName;
		o[QStringLiteral("safe")] = pr.window->HasSafeArea();
		if (pr.hasTile) {
			QJsonObject to;
			to[QStringLiteral("x")] = pr.tile.x();
			to[QStringLiteral("y")] = pr.tile.y();
			to[QStringLiteral("w")] = pr.tile.width();
			to[QStringLiteral("h")] = pr.tile.height();
			to[QStringLiteral("rot")] = pr.tileRot;
			to[QStringLiteral("blendL")] = pr.blendL;
			to[QStringLiteral("blendR")] = pr.blendR;
			to[QStringLiteral("blendT")] = pr.blendT;
			to[QStringLiteral("blendB")] = pr.blendB;
			to[QStringLiteral("kTLx")] = pr.kTLx;
			to[QStringLiteral("kTLy")] = pr.kTLy;
			to[QStringLiteral("kTRx")] = pr.kTRx;
			to[QStringLiteral("kTRy")] = pr.kTRy;
			to[QStringLiteral("kBLx")] = pr.kBLx;
			to[QStringLiteral("kBLy")] = pr.kBLy;
			to[QStringLiteral("kBRx")] = pr.kBRx;
			to[QStringLiteral("kBRy")] = pr.kBRy;
			to[QStringLiteral("gammaV")] = pr.gammaV;
			to[QStringLiteral("liftV")] = pr.liftV;
			to[QStringLiteral("gainV")] = pr.gainV;
			to[QStringLiteral("cMTx")] = pr.cMTx;
			to[QStringLiteral("cMTy")] = pr.cMTy;
			to[QStringLiteral("cMRx")] = pr.cMRx;
			to[QStringLiteral("cMRy")] = pr.cMRy;
			to[QStringLiteral("cMBx")] = pr.cMBx;
			to[QStringLiteral("cMBy")] = pr.cMBy;
			to[QStringLiteral("cMLx")] = pr.cMLx;
			to[QStringLiteral("cMLy")] = pr.cMLy;
			to[QStringLiteral("cCx")] = pr.cCx;
			to[QStringLiteral("cCy")] = pr.cCy;
			o[QStringLiteral("tile")] = to;
		}
		projArr.append(o);
	}

	/* 节目单（编排结果） */
	QJsonArray cueArr;
	for (const HfrCue &c : cues) {
		QJsonObject o;
		o[QStringLiteral("type")] = c.type;
		o[QStringLiteral("waitMs")] = c.waitMs;
		o[QStringLiteral("canvas")] = c.canvas;
		o[QStringLiteral("scene")] = c.scene;
		cueArr.append(o);
	}

	/* 多屏命名布局 */
	QJsonArray layoutArr;
	for (const HfrLayout &l : layouts) {
		QJsonObject lo;
		lo[QStringLiteral("name")] = l.name;
		QJsonArray itemArr;
		for (const HfrLayoutItem &it : l.items) {
			QJsonObject io;
			io[QStringLiteral("canvas")] = it.canvas;
			io[QStringLiteral("screen")] = it.screen;
			io[QStringLiteral("safe")] = it.safe;
			itemArr.append(io);
		}
		lo[QStringLiteral("items")] = itemArr;
		layoutArr.append(lo);
	}

	QJsonObject root;
	root[QStringLiteral("version")] = 3; /* v3: scenes 存场景内容（源/变换），兼容读取旧 v2 字符串数组 */
	root[QStringLiteral("canvases")] = canvasArr;
	root[QStringLiteral("projections")] = projArr;
	root[QStringLiteral("rundown")] = cueArr;
	root[QStringLiteral("rundownIndex")] = cueIndex;
	root[QStringLiteral("layouts")] = layoutArr;
	/* 界面页签记忆（0=画布 1=节目单 2=素材 3=输出） */
	root[QStringLiteral("uiTab")] = tabs ? tabs->currentIndex() : 0;

	/* 原子写入：先写临时文件再 rename（QSaveFile），避免断电/崩溃写坏配置；
	 * 保存前把上一份好配置备份为 .bak，读取失败时自动回退。 */
	if (QFile::exists(file)) {
		QFile::remove(file + QStringLiteral(".bak"));
		QFile::copy(file, file + QStringLiteral(".bak"));
	}
	const QByteArray data = QJsonDocument(root).toJson(QJsonDocument::Indented);
	QSaveFile sf(file);
	if (!sf.open(QIODevice::WriteOnly)) {
		SetStatus(QStringLiteral("保存配置失败：无法写入 %1").arg(file), 2);
		return;
	}
	if (sf.write(data) != data.size() || !sf.commit()) {
		SetStatus(QStringLiteral("保存配置失败：写入不完整"), 2);
	}
}

void HFRConsoleDock::LoadState()
{
	const QString file = StateFilePath();
	if (file.isEmpty() || !QFile::exists(file)) {
		return;
	}
	auto readDoc = [](const QString &path, QJsonDocument &out) -> bool {
		QFile f(path);
		if (!f.open(QIODevice::ReadOnly)) {
			return false;
		}
		out = QJsonDocument::fromJson(f.readAll());
		f.close();
		return out.isObject();
	};
	QJsonDocument doc;
	if (!readDoc(file, doc)) {
		/* 主文件损坏：回退备份 */
		if (!readDoc(file + QStringLiteral(".bak"), doc)) {
			SetStatus(QStringLiteral("配置读取失败（主文件与备份均损坏），使用默认配置"), 2);
			return;
		}
		SetStatus(QStringLiteral("配置主文件损坏，已从备份恢复"), 1);
	}

	loadingState = true;

	const QJsonArray canvasArr = doc.object().value(QStringLiteral("canvases")).toArray();
	for (const QJsonValue &v : canvasArr) {
		const QJsonObject o = v.toObject();
		const QString name = o.value(QStringLiteral("name")).toString();
		const uint32_t w = (uint32_t)o.value(QStringLiteral("w")).toInt(1280);
		const uint32_t h = (uint32_t)o.value(QStringLiteral("h")).toInt(720);
		if (name.isEmpty()) {
			continue;
		}
		struct obs_video_info ovi = {};
		obs_get_video_info(&ovi);
		ovi.base_width = w ? w : 1280;
		ovi.base_height = h ? h : 720;
		obs_canvas_t *cv = obs_canvas_create(name.toUtf8().constData(), &ovi, ACTIVATE | SCENE_REF);
		if (!cv) {
			continue;
		}
		canvases.append(cv);
		safeAreaPref.insert(cv, o.value(QStringLiteral("safe")).toBool(false));

		/* 拼接块定义（画布=墙，分辨率已在上面恢复） */
		QVector<HfrWallTile> tiles;
		for (const QJsonValue &tv : o.value(QStringLiteral("tiles")).toArray()) {
			const QJsonObject to = tv.toObject();
			HfrWallTile t;
			t.screen = to.value(QStringLiteral("screen")).toInt(-1);
			t.x = to.value(QStringLiteral("x")).toInt(0);
			t.y = to.value(QStringLiteral("y")).toInt(0);
			t.w = to.value(QStringLiteral("w")).toInt(0);
			t.h = to.value(QStringLiteral("h")).toInt(0);
			t.rot = to.value(QStringLiteral("rot")).toInt(0);
			t.blendL = to.value(QStringLiteral("blendL")).toInt(0);
			t.blendR = to.value(QStringLiteral("blendR")).toInt(0);
			t.blendT = to.value(QStringLiteral("blendT")).toInt(0);
			t.blendB = to.value(QStringLiteral("blendB")).toInt(0);
			t.kTLx = to.value(QStringLiteral("kTLx")).toInt(0);
			t.kTLy = to.value(QStringLiteral("kTLy")).toInt(0);
			t.kTRx = to.value(QStringLiteral("kTRx")).toInt(0);
			t.kTRy = to.value(QStringLiteral("kTRy")).toInt(0);
			t.kBLx = to.value(QStringLiteral("kBLx")).toInt(0);
			t.kBLy = to.value(QStringLiteral("kBLy")).toInt(0);
			t.kBRx = to.value(QStringLiteral("kBRx")).toInt(0);
			t.kBRy = to.value(QStringLiteral("kBRy")).toInt(0);
			t.gammaV = to.value(QStringLiteral("gammaV")).toDouble(1.0);
			t.liftV = to.value(QStringLiteral("liftV")).toDouble(0.0);
			t.gainV = to.value(QStringLiteral("gainV")).toDouble(1.0);
			t.cMTx = to.value(QStringLiteral("cMTx")).toInt(0);
			t.cMTy = to.value(QStringLiteral("cMTy")).toInt(0);
			t.cMRx = to.value(QStringLiteral("cMRx")).toInt(0);
			t.cMRy = to.value(QStringLiteral("cMRy")).toInt(0);
			t.cMBx = to.value(QStringLiteral("cMBx")).toInt(0);
			t.cMBy = to.value(QStringLiteral("cMBy")).toInt(0);
			t.cMLx = to.value(QStringLiteral("cMLx")).toInt(0);
			t.cMLy = to.value(QStringLiteral("cMLy")).toInt(0);
			t.cCx = to.value(QStringLiteral("cCx")).toInt(0);
			t.cCy = to.value(QStringLiteral("cCy")).toInt(0);
			if (t.screen >= 0 && t.w > 0 && t.h > 0) {
				tiles.push_back(t);
			}
		}
		if (!tiles.isEmpty()) {
			wallTiles.insert(cv, tiles);
		}

		const bool isTest = o.value(QStringLiteral("test")).toBool(false);
		const QString curName = o.value(QStringLiteral("cur")).toString();
		const QJsonArray scenesArr = o.value(QStringLiteral("scenes")).toArray();

		if (scenesArr.isEmpty() && isTest) {
			/* 兼容旧存档：只有 test 标记 */
			SetCanvasTestColor(cv);
			testSceneAdded.insert(cv, true);
		} else {
			for (const QJsonValue &sv : scenesArr) {
				QString sname;
				QJsonArray itemsArr;
				if (sv.isString()) {
					sname = sv.toString(); /* 旧格式：只有场景名 */
				} else {
					const QJsonObject so = sv.toObject();
					sname = so.value(QStringLiteral("name")).toString();
					itemsArr = so.value(QStringLiteral("items")).toArray();
				}
				if (sname.isEmpty()) {
					continue;
				}
				obs_scene_t *sc = obs_canvas_scene_create(cv, sname.toUtf8().constData());
				if (!sc) {
					continue;
				}
				if (!itemsArr.isEmpty()) {
					/* 新格式：恢复场景内容（源 + 变换 + 裁剪） */
					RestoreSceneItems(sc, itemsArr);
					if (isTest) {
						testSceneAdded.insert(cv, true);
					}
				} else if (isTest && sname.endsWith(QStringLiteral(" · 测试色块"))) {
					/* 旧格式兼容：测试色场景重建色块 */
					AddColorIntoScene(cv, sc, sname.left(sname.length() - QStringLiteral("块").length()) + QStringLiteral("色"));
					testSceneAdded.insert(cv, true);
				}
				if (!curName.isEmpty() && sname == curName) {
					obs_canvas_set_channel(cv, 0, obs_scene_get_source(sc));
				}
			}
		}
	}

	RefreshCanvasList();

	/* 节目单编排恢复 */
	cues.clear();
	const QJsonArray cueArr = doc.object().value(QStringLiteral("rundown")).toArray();
	for (const QJsonValue &v : cueArr) {
		const QJsonObject o = v.toObject();
		HfrCue c;
		c.type = o.value(QStringLiteral("type")).toInt(0);
		c.waitMs = o.value(QStringLiteral("waitMs")).toInt(0);
		c.canvas = o.value(QStringLiteral("canvas")).toString();
		c.scene = o.value(QStringLiteral("scene")).toString();
		if (c.type == 6) {
			if (!c.canvas.isEmpty()) {
				cues.push_back(c);
			}
		} else if (c.type >= 1 && c.type <= 5) {
			cues.push_back(c); /* 特殊条目无画布/场景 */
		} else if (!c.canvas.isEmpty() && !c.scene.isEmpty()) {
			cues.push_back(c);
		}
	}
	cueIndex = doc.object().value(QStringLiteral("rundownIndex")).toInt(cues.isEmpty() ? -1 : 0);
	if (cueIndex >= cues.size()) {
		cueIndex = cues.isEmpty() ? -1 : cues.size() - 1;
	}
	RefreshCueList();

	/* 页签记忆：恢复上次所在页签（越界则回第 1 页） */
	if (tabs) {
		const int uiTab = doc.object().value(QStringLiteral("uiTab")).toInt(0);
		tabs->setCurrentIndex(uiTab >= 0 && uiTab < tabs->count() ? uiTab : 0);
	}

	const QJsonArray projArr = doc.object().value(QStringLiteral("projections")).toArray();
	const auto screens = QGuiApplication::screens();
	for (const QJsonValue &v : projArr) {
		const QJsonObject o = v.toObject();
		const QString cname = o.value(QStringLiteral("canvas")).toString();
		int idx = o.value(QStringLiteral("screen")).toInt(-1);
		const QString savedScreenName = o.value(QStringLiteral("screenName")).toString();
		const bool safe = o.value(QStringLiteral("safe")).toBool(false);

		obs_canvas_t *cv = nullptr;
		if (cname == QStringLiteral("Main") || cname.isEmpty()) {
			cv = obs_get_main_canvas();
		} else {
			for (obs_canvas_t *c : canvases) {
				if (c && cname == QString::fromUtf8(obs_canvas_get_name(c))) {
					cv = c;
					break;
				}
			}
		}
		if (!cv) {
			continue;
		}
		/* 显示器顺序可能变化：优先按名称匹配，其次按索引 */
		if (!savedScreenName.isEmpty()) {
			for (int i = 0; i < screens.size(); i++) {
				if (screens.at(i)->name() == savedScreenName) {
					idx = i;
					break;
				}
			}
		}
		if (idx < 0 || idx >= screens.size()) {
			continue;
		}
		const QJsonObject tileObj = o.value(QStringLiteral("tile")).toObject();
		if (!tileObj.isEmpty()) {
			const QRect tr(tileObj.value(QStringLiteral("x")).toInt(0),
				       tileObj.value(QStringLiteral("y")).toInt(0),
				       tileObj.value(QStringLiteral("w")).toInt(0),
				       tileObj.value(QStringLiteral("h")).toInt(0));
			if (tr.width() > 0 && tr.height() > 0) {
				HfrWallTile t;
				t.screen = idx;
				t.x = tr.x();
				t.y = tr.y();
				t.w = tr.width();
				t.h = tr.height();
				t.rot = tileObj.value(QStringLiteral("rot")).toInt(0);
				t.blendL = tileObj.value(QStringLiteral("blendL")).toInt(0);
				t.blendR = tileObj.value(QStringLiteral("blendR")).toInt(0);
				t.blendT = tileObj.value(QStringLiteral("blendT")).toInt(0);
				t.blendB = tileObj.value(QStringLiteral("blendB")).toInt(0);
				t.kTLx = tileObj.value(QStringLiteral("kTLx")).toInt(0);
				t.kTLy = tileObj.value(QStringLiteral("kTLy")).toInt(0);
				t.kTRx = tileObj.value(QStringLiteral("kTRx")).toInt(0);
				t.kTRy = tileObj.value(QStringLiteral("kTRy")).toInt(0);
				t.kBLx = tileObj.value(QStringLiteral("kBLx")).toInt(0);
				t.kBLy = tileObj.value(QStringLiteral("kBLy")).toInt(0);
				t.kBRx = tileObj.value(QStringLiteral("kBRx")).toInt(0);
				t.kBRy = tileObj.value(QStringLiteral("kBRy")).toInt(0);
				t.gammaV = tileObj.value(QStringLiteral("gammaV")).toDouble(1.0);
				t.liftV = tileObj.value(QStringLiteral("liftV")).toDouble(0.0);
				t.gainV = tileObj.value(QStringLiteral("gainV")).toDouble(1.0);
				t.cMTx = tileObj.value(QStringLiteral("cMTx")).toInt(0);
				t.cMTy = tileObj.value(QStringLiteral("cMTy")).toInt(0);
				t.cMRx = tileObj.value(QStringLiteral("cMRx")).toInt(0);
				t.cMRy = tileObj.value(QStringLiteral("cMRy")).toInt(0);
				t.cMBx = tileObj.value(QStringLiteral("cMBx")).toInt(0);
				t.cMBy = tileObj.value(QStringLiteral("cMBy")).toInt(0);
				t.cMLx = tileObj.value(QStringLiteral("cMLx")).toInt(0);
				t.cMLy = tileObj.value(QStringLiteral("cMLy")).toInt(0);
				t.cCx = tileObj.value(QStringLiteral("cCx")).toInt(0);
				t.cCy = tileObj.value(QStringLiteral("cCy")).toInt(0);
				ProjectTileToMonitor(cv, t);
				continue;
			}
		}
		ProjectCanvasToMonitor(cv, idx);
		if (!projections.isEmpty()) {
			projections.last().window->SetSafeArea(safe);
		}
	}

	/* 多屏命名布局恢复 */
	layouts.clear();
	const QJsonArray layArr = doc.object().value(QStringLiteral("layouts")).toArray();
	for (const QJsonValue &v : layArr) {
		const QJsonObject lo = v.toObject();
		HfrLayout lay;
		lay.name = lo.value(QStringLiteral("name")).toString();
		if (lay.name.isEmpty()) {
			continue;
		}
		for (const QJsonValue &iv : lo.value(QStringLiteral("items")).toArray()) {
			const QJsonObject io = iv.toObject();
			HfrLayoutItem it;
			it.canvas = io.value(QStringLiteral("canvas")).toString();
			it.screen = io.value(QStringLiteral("screen")).toInt(-1);
			it.safe = io.value(QStringLiteral("safe")).toBool(false);
			if (!it.canvas.isEmpty()) {
				lay.items.push_back(it);
			}
		}
		layouts.push_back(lay);
	}
	RefreshLayoutCombo();

	loadingState = false;
	HfrLog("state loaded");
}

void HFRConsoleDock::RemoveSelectedCanvas()
{
	int row = canvasList->currentRow();
	obs_canvas_t *cv = CanvasFromRow(row);
	if (!cv) {
		return;
	}
	if (cv == obs_get_main_canvas()) {
		SetStatus(QStringLiteral("主画布不可删除"), 1);
		return;
	}
	if (!canvases.contains(cv)) {
		return;
	}
	for (int i = projections.size() - 1; i >= 0; i--) {
		if (projections[i].canvas == cv) {
			if (projections[i].window) {
				projections[i].window->close();
			}
			projections.removeAt(i);
		}
	}
	canvases.removeAll(cv);
	testSceneAdded.remove(cv);
	safeAreaPref.remove(cv);
	TeardownUserCanvas(cv);
	RefreshCanvasList();
	SaveState();
	HfrLog("removed canvas");
	status->setText(QStringLiteral("已删除画布"));
}

void HFRConsoleDock::SetCanvasTestColor(obs_canvas_t *cv)
{
	if (!cv || cv == obs_get_main_canvas()) {
		SetStatus(QStringLiteral("主画布由主界面管理，测试色仅用于用户画布"), 1);
		return;
	}
	/* 已填充检测：直接看通道 0 是否已是本画布的测试场景（比内存标记更可靠） */
	const QByteArray canvasName = obs_canvas_get_name(cv);
	const std::string testSceneName = canvasName.toStdString() + " · 测试色块";
	if (obs_source_t *cur = obs_canvas_get_channel(cv, 0)) {
		const char *curName = obs_source_get_name(cur);
		if (curName && testSceneName == curName) {
			status->setText(QStringLiteral("该画布已填充测试色"));
			return;
		}
	}

	/* 名称按画布唯一化，避免多画布重名导致场景创建失败（表现为黑屏） */
	obs_scene_t *sc = obs_canvas_scene_create(cv, testSceneName.c_str());
	if (!sc) {
		SetStatus(QStringLiteral("创建测试场景失败（名称冲突？）"), 2);
		HfrLog("test scene create FAILED");
		return;
	}

	/* 色块尺寸 = 该画布基础分辨率，并拉伸铺满，任何分辨率/宽高比都正确 */
	struct obs_video_info ovi = {};
	uint32_t w = 1280, h = 720;
	if (obs_canvas_get_video_info(cv, &ovi) && ovi.base_width && ovi.base_height) {
		w = ovi.base_width;
		h = ovi.base_height;
	}

	AddColorIntoScene(cv, sc, canvasName + QStringLiteral(" · 测试色"));

	obs_canvas_set_channel(cv, 0, obs_scene_get_source(sc));
	testSceneAdded.insert(cv, true);
	HfrLog("test color scene added to canvas (sized to canvas)");
	status->setText(QStringLiteral("已填充测试色（%1x%2 铺满）").arg(w).arg(h));
}

void HFRConsoleDock::SetTestColorOnSelected()
{
	obs_canvas_t *cv = CanvasFromRow(canvasList->currentRow());
	if (!cv) {
		SetStatus(QStringLiteral("请先点选画布"), 1);
		return;
	}
	SetCanvasTestColor(cv);
	status->setText(QStringLiteral("已填充测试色：%1").arg(QString::fromUtf8(obs_canvas_get_name(cv))));
}

void HFRConsoleDock::ProjectCanvasToMonitor(obs_canvas_t *cv, int mRow)
{
	const auto screens = QGuiApplication::screens();
	if (!cv || mRow < 0 || mRow >= screens.size()) {
		SetStatus(QStringLiteral("画布或显示器编号无效"), 1);
		return;
	}
	/* 同画布+同屏已在投 → 前置即可 */
	for (HFRProjection &pr : projections) {
		if (pr.window && pr.canvas == cv && pr.screenIndex == mRow) {
			pr.window->raise();
			pr.window->activateWindow();
			status->setText(QStringLiteral("已在前台：%1 → 屏%2")
						.arg(QString::fromUtf8(obs_canvas_get_name(cv)))
						.arg(mRow));
			return;
		}
	}
	HFRProjection pr;
	pr.window = new HFRCanvasProjector(cv, screens.at(mRow), nullptr);
	pr.window->SetSafeArea(safeAreaPref.value(cv, false));
	pr.canvas = cv;
	pr.screenIndex = mRow;
	projections.append(pr);
	SaveState();
	HfrLog("projected canvas to screen");
	status->setText(QStringLiteral("已投影 %1 → 屏%2（投影 %3 个）")
				.arg(QString::fromUtf8(obs_canvas_get_name(cv)))
				.arg(mRow)
				.arg(projections.size()));
}

void HFRConsoleDock::ProjectTileToMonitor(obs_canvas_t *cv, const HfrWallTile &t)
{
	const auto screens = QGuiApplication::screens();
	const QRect tile(t.x, t.y, t.w, t.h);
	if (!cv || t.screen < 0 || t.screen >= screens.size() || t.w <= 0 || t.h <= 0) {
		SetStatus(QStringLiteral("拼接块无效（画布/屏幕/尺寸）"), 1);
		return;
	}
	HFRProjection pr;
	pr.window = new HFRCanvasProjector(cv, screens.at(t.screen), nullptr, tile, t.rot, t.blendL,
					   t.blendR, t.blendT, t.blendB, &t);
	pr.window->SetSafeArea(false);
	pr.canvas = cv;
	pr.screenIndex = t.screen;
	pr.tile = tile;
	pr.hasTile = true;
	pr.tileRot = t.rot;
	pr.blendL = t.blendL;
	pr.blendR = t.blendR;
	pr.blendT = t.blendT;
	pr.blendB = t.blendB;
	pr.kTLx = t.kTLx;
	pr.kTLy = t.kTLy;
	pr.kTRx = t.kTRx;
	pr.kTRy = t.kTRy;
	pr.kBLx = t.kBLx;
	pr.kBLy = t.kBLy;
	pr.kBRx = t.kBRx;
	pr.kBRy = t.kBRy;
	pr.hasKeystone = t.HasKeystone();
	pr.gammaV = t.gammaV;
	pr.liftV = t.liftV;
	pr.gainV = t.gainV;
	pr.hasColorAdj = t.HasColorAdjust();
	pr.cMTx = t.cMTx;
	pr.cMTy = t.cMTy;
	pr.cMRx = t.cMRx;
	pr.cMRy = t.cMRy;
	pr.cMBx = t.cMBx;
	pr.cMBy = t.cMBy;
	pr.cMLx = t.cMLx;
	pr.cMLy = t.cMLy;
	pr.cCx = t.cCx;
	pr.cCy = t.cCy;
	pr.hasCurve = t.HasCurve();
	projections.append(pr);
}

void HFRConsoleDock::ApplySplice(obs_canvas_t *cv, const QVector<HfrWallTile> &tiles)
{
	if (!cv) {
		return;
	}
	/* 同一画布的旧投影先撤掉，避免叠加/重复窗口 */
	for (int i = projections.size() - 1; i >= 0; i--) {
		if (projections[i].canvas == cv) {
			if (projections[i].window) {
				projections[i].window->close();
			}
			projections.removeAt(i);
		}
	}
	wallTiles.insert(cv, tiles);

	const auto screens = QGuiApplication::screens();
	int okCount = 0, failCount = 0;
	for (const HfrWallTile &t : tiles) {
		if (t.screen < 0 || t.screen >= screens.size() || t.w <= 0 || t.h <= 0) {
			failCount++;
			continue;
		}
		ProjectTileToMonitor(cv, t);
		okCount++;
	}
	SaveState();
	if (failCount) {
		SetStatus(QStringLiteral("拼接投影：%1 块成功，%2 块失败（屏幕/尺寸无效）").arg(okCount).arg(failCount), 1);
	} else {
		SetStatus(QStringLiteral("拼接投影：%1 块已上屏（画布分辨率=墙分辨率）")
				  .arg(okCount));
	}
	HfrLog("splice projection applied");
}

void HFRConsoleDock::OpenSpliceDialog()
{
	obs_canvas_t *cv = CanvasFromRow(canvasList->currentRow());
	if (!cv) {
		SetStatus(QStringLiteral("请先点选画布：画布分辨率 = 拼接墙分辨率"), 1);
		return;
	}
	HFRSpliceDialog dlg(cv, wallTiles.value(cv),
			    [this](obs_canvas_t *c, const QVector<HfrWallTile> &tiles) { ApplySplice(c, tiles); },
			    this);
	dlg.exec();
	SaveState();
}

void HFRConsoleDock::ProjectSelected()
{
	int cRow = canvasList->currentRow();
	int mRow = monitorList->currentRow();
	obs_canvas_t *cv = CanvasFromRow(cRow);
	if (!cv) {
		SetStatus(QStringLiteral("请先点选画布"), 1);
		return;
	}
	ProjectCanvasToMonitor(cv, mRow);
}

void HFRConsoleDock::ToggleSafeAreaOnSelected()
{
	obs_canvas_t *cv = CanvasFromRow(canvasList->currentRow());
	if (!cv) {
		SetStatus(QStringLiteral("请先点选画布"), 1);
		return;
	}
	bool on = !safeAreaPref.value(cv, false);
	safeAreaPref.insert(cv, on);
	int n = 0;
	for (HFRProjection &pr : projections) {
		if (pr.canvas == cv && pr.window) {
			pr.window->SetSafeArea(on);
			n++;
		}
	}
	status->setText(QStringLiteral("%1 安全区：%2（作用于 %3 个投影；新投影自动沿用）")
				.arg(QString::fromUtf8(obs_canvas_get_name(cv)))
				.arg(on ? QStringLiteral("开") : QStringLiteral("关"))
				.arg(n));
	SaveState();
	HfrLog(on ? "safe area ON" : "safe area OFF");
}

void HFRConsoleDock::StopProjectionOfSelected()
{
	obs_canvas_t *cv = CanvasFromRow(canvasList->currentRow());
	if (!cv) {
		SetStatus(QStringLiteral("请先点选画布"), 1);
		return;
	}
	int before = projections.size();
	for (int i = projections.size() - 1; i >= 0; i--) {
		HFRProjection &pr = projections[i];
		if (pr.canvas == cv) {
			if (pr.window) {
				pr.window->close();
			}
			projections.removeAt(i);
		}
	}
	status->setText(QStringLiteral("已停止 %1 的投影（%2 → %3 个）")
				.arg(QString::fromUtf8(obs_canvas_get_name(cv)))
				.arg(before - projections.size())
				.arg(projections.size()));
	SaveState();
}

void HFRConsoleDock::CloseAllProjectors()
{
	for (HFRProjection &pr : projections) {
		if (pr.window) {
			pr.window->close();
		}
	}
	projections.clear();
	SaveState();
}

/* 找/建画布上的"⛔ 黑场"场景（铺满纯黑），返回场景源（借用） */
static obs_source_t *FindOrCreateBlackoutScene(obs_canvas_t *cv)
{
	if (!cv) {
		return nullptr;
	}
	std::vector<obs_source_t *> scenes;
	obs_canvas_enum_scenes(cv, CollectSceneSourcesCallback, &scenes);
	for (obs_source_t *s : scenes) {
		if (s && QString::fromUtf8(obs_source_get_name(s)) == QStringLiteral("⛔ 黑场")) {
			return s;
		}
	}
	obs_scene_t *sc = obs_canvas_scene_create(cv, "⛔ 黑场");
	if (!sc) {
		return nullptr;
	}
	struct obs_video_info ovi = {};
	uint32_t w = 1920, h = 1080;
	if (obs_canvas_get_video_info(cv, &ovi) && ovi.base_width && ovi.base_height) {
		w = ovi.base_width;
		h = ovi.base_height;
	}
	obs_data_t *s = obs_data_create();
	obs_data_set_int(s, "color", 0xFF000000);
	obs_data_set_int(s, "width", (long long)w);
	obs_data_set_int(s, "height", (long long)h);
	obs_source_t *cs = obs_source_create("color_source_v3", "黑场色块", s, nullptr);
	obs_data_release(s);
	if (cs) {
		obs_scene_add(sc, cs);
		obs_source_release(cs);
	}
	return obs_scene_get_source(sc);
}

void HFRConsoleDock::ToggleBlackout()
{
	QVector<obs_canvas_t *> all;
	if (obs_canvas_t *main = obs_get_main_canvas()) {
		all.push_back(main);
		obs_canvas_release(main);
	}
	for (obs_canvas_t *cv : canvases) {
		if (cv) {
			all.push_back(cv);
		}
	}

	if (!blackoutActive) {
		blackoutPrev.clear();
		int ok = 0;
		for (obs_canvas_t *cv : all) {
			obs_source_t *black = FindOrCreateBlackoutScene(cv);
			if (!black) {
				continue;
			}
			obs_source_t *prev = obs_canvas_get_channel(cv, 0);
			if (prev) {
				obs_source_get_ref(prev);
				blackoutPrev.insert(cv, prev);
			}
			obs_canvas_set_channel(cv, 0, black);
			ok++;
		}
		blackoutActive = true;
		SetStatus(QStringLiteral("黑场：%1 路画布已切黑（再点「恢复画面」）").arg(ok));
	} else {
		int ok = 0;
		for (obs_canvas_t *cv : all) {
			obs_source_t *prev = blackoutPrev.value(cv, nullptr);
			if (prev) {
				obs_canvas_set_channel(cv, 0, prev);
				obs_source_release(prev);
				ok++;
			}
		}
		blackoutPrev.clear();
		blackoutActive = false;
		SetStatus(QStringLiteral("已恢复画面：%1 路画布").arg(ok));
	}
	UpdateSceneListForSelection();
	SaveState();
}

/* ---------------- 多屏布局（命名保存 / 一键还原） ---------------- */

void HFRConsoleDock::RefreshLayoutCombo()
{
	if (!layoutCombo) {
		return;
	}
	const int keep = layoutCombo->currentIndex();
	layoutCombo->clear();
	for (const HfrLayout &l : layouts) {
		layoutCombo->addItem(l.name);
	}
	if (keep >= 0 && keep < layoutCombo->count()) {
		layoutCombo->setCurrentIndex(keep);
	}
}

void HFRConsoleDock::SaveCurrentLayout()
{
	if (projections.isEmpty()) {
		SetStatus(QStringLiteral("当前没有投影：先投影几路，再保存为布局"), 1);
		return;
	}
	bool ok = false;
	const QString name = QInputDialog::getText(this, QStringLiteral("保存布局"), QStringLiteral("布局名称"),
						   QLineEdit::Normal,
						   QStringLiteral("布局 %1").arg(layouts.size() + 1), &ok);
	if (!ok || name.trimmed().isEmpty()) {
		return;
	}
	HfrLayout lay;
	lay.name = name.trimmed();
	for (const HFRProjection &pr : projections) {
		if (!pr.window || !pr.canvas) {
			continue;
		}
		HfrLayoutItem it;
		it.canvas = QString::fromUtf8(obs_canvas_get_name(pr.canvas));
		it.screen = pr.screenIndex;
		it.safe = pr.window->HasSafeArea();
		lay.items.push_back(it);
	}
	if (lay.items.isEmpty()) {
		SetStatus(QStringLiteral("当前投影不可用：未保存布局"), 1);
		return;
	}
	for (int i = 0; i < layouts.size(); i++) {
		if (layouts[i].name == lay.name) {
			layouts.removeAt(i);
			break;
		}
	}
	layouts.push_back(lay);
	RefreshLayoutCombo();
	if (layoutCombo) {
		layoutCombo->setCurrentIndex(layoutCombo->count() - 1);
	}
	SaveState();
	SetStatus(QStringLiteral("已保存布局「%1」（%2 路投影）").arg(lay.name).arg(lay.items.size()));
}

void HFRConsoleDock::ApplySelectedLayout()
{
	const int idx = layoutCombo ? layoutCombo->currentIndex() : -1;
	if (idx < 0 || idx >= layouts.size()) {
		SetStatus(QStringLiteral("请先选择布局"), 1);
		return;
	}
	const HfrLayout lay = layouts.at(idx);
	CloseAllProjectors();
	const auto screens = QGuiApplication::screens();
	int okCount = 0, failCount = 0;
	for (const HfrLayoutItem &it : lay.items) {
		bool isMain = false;
		obs_canvas_t *cv = FindCanvasByNameForCue(canvases, it.canvas, &isMain);
		if (!cv || it.screen < 0 || it.screen >= screens.size()) {
			failCount++;
			if (isMain) {
				obs_canvas_release(cv);
			}
			continue;
		}
		ProjectCanvasToMonitor(cv, it.screen);
		if (it.safe && !projections.isEmpty() && projections.last().window) {
			projections.last().window->SetSafeArea(true);
		}
		if (isMain) {
			obs_canvas_release(cv);
		}
		okCount++;
	}
	SaveState();
	if (failCount) {
		SetStatus(QStringLiteral("已应用布局「%1」：%2 路成功，%3 路失败（画布/屏幕不存在）")
				  .arg(lay.name)
				  .arg(okCount)
				  .arg(failCount),
			  1);
	} else {
		SetStatus(QStringLiteral("已应用布局「%1」：%2 路投影").arg(lay.name).arg(okCount));
	}
}

void HFRConsoleDock::DeleteSelectedLayout()
{
	const int idx = layoutCombo ? layoutCombo->currentIndex() : -1;
	if (idx < 0 || idx >= layouts.size()) {
		SetStatus(QStringLiteral("请先选择布局"), 1);
		return;
	}
	const QString name = layouts[idx].name;
	layouts.removeAt(idx);
	RefreshLayoutCombo();
	SaveState();
	SetStatus(QStringLiteral("已删除布局「%1」").arg(name));
}