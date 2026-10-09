#pragma once
/* HFRDeck —— 自研 PPT 引擎（M0）文档模型与 OOXML 解析
 * 依赖：zlib（OBS 依赖包已提供）。不依赖 Qt / OBS，便于独立验证。 */
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace hfr {

struct Color {
	bool valid = false;
	uint32_t argb = 0xFF000000u;
};

/* 一级文本样式（来自母版 txStyles / 版式 lstStyle） */
struct LevelStyle {
	double sizePt = 0;
	bool bold = false;
	bool italic = false;
	Color color;
	std::wstring bulletChar;
	bool bulletNone = false;
	int marL = 0;
	double lnSpcPct = 0;
};

struct Run {
	std::wstring text;
	double sizePt = 18.0;
	bool hasSize = false;
	bool bold = false;
	bool italic = false;
	bool underline = false;
	Color color;
	std::wstring latin;   /* 西文字体 */
	std::wstring ea;      /* 东亚字体 */
};

struct Para {
	std::vector<Run> runs;
	int align = 0;        /* 0=left 1=center 2=right 3=justify */
	int level = 0;
	bool bullet = false;
	std::wstring bulletChar;   /* 非空 = 使用该符号作为项目符号 */
	bool bulletExplicit = false;
	int marL = 0;              /* 左缩进（EMU） */
	double lineSpacing = 1.0;
};

/* 一个动画效果（来自 p:timing） */
struct Anim {
	int shapeId = 0;             /* 目标形状 id（p:cNvPr@id） */
	int shapeIndex = -1;         /* 解析后映射到 shapes 下标 */
	std::wstring presetClass;    /* entr / exit / emph / path */
	int presetId = 0;
	std::wstring filter;         /* fade / wipe(...) / ... */
	std::wstring transition;     /* in / out */
	double durMs = 500.0;
	double delayMs = 0.0;
};

struct GradientStop {
	double pos = 0.0; /* 0..1 */
	Color color;
};

struct TableCell {
	std::vector<Para> paras;
	bool hasFill = false;
	Color fill;
	bool merged = false;
};

struct Shape {
	enum class Kind { Unknown, AutoShape, TextBox, Placeholder, Picture, Group, Table, Chart, Media };
	Kind kind = Kind::Unknown;
	int id = 0;                  /* p:cNvPr@id */
	std::wstring name;
	int64_t x = 0, y = 0, w = 0, h = 0; /* EMU */
	int64_t rot = 0;                    /* 1/60000 度 */
	bool hasFill = false;
	Color fill;
	bool hasGradient = false;
	std::vector<GradientStop> gradStops;
	double gradAngleDeg = 90.0; /* 0=向右，90=向下（由 lin@ang 换算） */
	bool hasLine = false;
	Color line;
	int64_t lineW = 9525;               /* EMU, 默认 0.75pt */
	std::wstring prstGeom;              /* rect / roundRect / ellipse ... */
	std::vector<Para> paras;
	std::wstring imageRelId;
	std::vector<uint8_t> imageData;     /* 已解析的图片字节 */
	std::wstring imageExt;
	bool hasImage = false;
	bool isPlaceholder = false;
	std::wstring phType;
	int phIdx = -1;
	std::vector<Shape> children;        /* 组合 */

	/* 表格（kind == Table） */
	std::vector<double> colW;                  /* 列宽（EMU） */
	std::vector<double> rowH;                  /* 行高（EMU） */
	std::vector<std::vector<TableCell>> cells; /* [row][col] */
};

struct Slide {
	std::vector<Shape> shapes;
	std::wstring notes;
	/* 动画步骤：外层 vector = 每次"点击"推进的一步 */
	std::vector<std::vector<Anim>> animSteps;

	/* 页间切换（p:transition） */
	std::wstring transType;   /* fade / push / wipe / cover / dissolve / none */
	std::wstring transDir;    /* l / r / u / d（push/wipe/cover） */
	double transMs = 500.0;   /* 时长（按 spd 换算） */
	bool transAdvClick = true;
};

struct Deck {
	int64_t slideW = 12192000, slideH = 6858000; /* EMU */
	std::vector<Slide> slides;
	std::wstring majorFont, minorFont;
	std::map<std::wstring, std::wstring> themeColors; /* dk1/lt1/accent1.. */
	std::wstring officeThemeName;
	std::wstring debugInfo; /* 解析诊断（布局继承等） */
	/* 文本样式表：title / body / other → 9 级 */
	std::map<std::wstring, std::vector<LevelStyle>> textStyles;
};

/* 解析 pptx。失败时返回 false 并写入 err。 */
bool LoadDeck(const std::wstring &pptxPath, Deck &out, std::wstring &err);

/* 调试用：把 Deck 摘要成文本 */
std::string DeckSummary(const Deck &deck);

} // namespace hfr