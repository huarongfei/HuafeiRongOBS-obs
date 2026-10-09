#pragma once
/* HFRDeckRender —— 自研引擎 M0 渲染层（CPU 光栅化，供 OBS 纹理上传与离线预览共用）
 * 只依赖 HFRDeck 模型与 Win32 GDI，不依赖 OBS/Qt。 */
#include "HFRDeck.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace hfr {

enum class ItemKind { Rect, Picture, Text, Table, Gradient };

struct Item {
	ItemKind kind = ItemKind::Rect;
	int x = 0, y = 0, w = 0, h = 0;        /* 目标空间像素 */
	int rot60k = 0;                        /* 1/60000 度 */
	int shapeIndex = -1;                   /* 来源形状下标（动画用） */
	uint32_t color = 0xFF000000u;          /* Rect 用 */
	std::vector<uint8_t> pix;              /* Text: BGRA 块；Picture: 原始编码字节 */
	int texW = 0, texH = 0;                /* Text: 块尺寸 */
	std::wstring ext;                      /* Picture: 扩展名 */
	std::wstring debug;                    /* 便于排障：形状名/文本摘要 */
};

struct RenderSlide {
	int width = 0, height = 0;
	std::vector<Item> items;
};

/* 把第 slideIndex 页构建为可渲染项（EMU → 目标像素；文本在 CPU 上光栅化） */
bool BuildSlide(const Deck &deck, int slideIndex, int targetW, int targetH, RenderSlide &out, std::wstring &err);

/* 离线合成预览（白底；图片以浅灰占位），用于不经 OBS 的视觉验证 */
void CompositeToBgra(const RenderSlide &slide, std::vector<uint8_t> &bgra);

/* 写出 32 位 BMP（BGRA，自顶向下） */
bool WriteBmp32(const std::wstring &path, int w, int h, const std::vector<uint8_t> &bgra);

} // namespace hfr
