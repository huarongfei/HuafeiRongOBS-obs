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
	bool strike = false;      /* a:rPr@strike */
	int baseline = 0;         /* a:rPr@baseline（1/1000 %，正=上标 负=下标） */
	double spcPt = 0.0;       /* a:rPr@spc（1/100 pt 字距） */
	bool highlight = false;   /* a:highlight */
	Color highlightColor;
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
	double spaceBeforePt = 0.0; /* 段前间距（pt，a:spcBef） */
	double spaceAfterPt = 0.0;  /* 段后间距（pt，a:spcAft） */
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
	/* p:animMotion 路径运动（v1）：归一化折线点，坐标以目标形状宽高为 1 单位 */
	bool hasPath = false;
	std::vector<double> pathX;
	std::vector<double> pathY;
	std::wstring pathPtsTypes;   /* 可选：p:animMotion@ptsTypes 原文（v1 仅记录） */
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
	int anchor = 0; /* 0=top 1=middle 2=bottom (a:tcPr@anchor) */
	int gridSpan = 1, rowSpan = 1; /* 合并跨度 */
	bool hMerge = false, vMerge = false; /* 被合并覆盖的续格（不绘制） */
	double marL = 0.0, marR = 0.0, marT = 0.0, marB = 0.0; /* EMU */
	bool borderL = false, borderR = false, borderT = false, borderB = false;
	Color borderColor;
	double borderWEmu = 12700.0; /* 1pt */
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
	double fontScale = 1.0;     /* a:normAutofit@fontScale（自动缩小字号） */
	bool noWrap = false;        /* a:bodyPr@wrap="none"（不换行） */
	double bright = 0.0;        /* 图片亮度 -1..1（a:lum@bright /100000） */
	double contrast = 0.0;      /* 图片对比度 -1..1（a:lum@contrast /100000） */
	/* 外阴影（a:outerShdw，简化渲染：偏移半透明矩形，不做高斯模糊） */
	bool hasShadow = false;
	double shadowDist = 0.0;    /* EMU */
	double shadowDirDeg = 45.0;
	double shadowAlpha = 0.5;
	Color shadowColor;
	bool hasLine = false;
	Color line;
	int64_t lineW = 9525;               /* EMU, 默认 0.75pt */
	/* a:bodyPr 文本内边距（EMU；PowerPoint 默认 0.1/0.05 英寸） */
	int64_t lIns = 91440, tIns = 45720, rIns = 91440, bIns = 45720;
	std::wstring prstGeom;              /* rect / roundRect / ellipse ... */
	std::wstring unsupported;           /* 非空 = 暂不支持的复杂对象（媒体/OLE），渲染为占位提示 */

	/* SmartArt（简化渲染）：从 diagram 数据模型取节点文本与父子关系，做简易树形布局 */
	bool isSmartArt = false;
	struct SmartNode {
		std::wstring text;
		int parent = -1;
	};
	std::vector<SmartNode> smartNodes;
	std::vector<Para> paras;
	std::wstring imageRelId;
	std::vector<uint8_t> imageData;     /* 已解析的图片字节 */
	std::wstring imageExt;
	bool hasImage = false;
	/* 图片裁剪（a:srcRect，单位 1/1000 %，可负）：左/上/右/下相对原图的比例 */
	double cropL = 0.0, cropT = 0.0, cropR = 0.0, cropB = 0.0;
	bool hasCrop = false;
	bool flipH = false; /* a:xfrm flipH（镜像） */
	bool flipV = false;
	bool isPlaceholder = false;
	std::wstring phType;
	int phIdx = -1;
	std::vector<Shape> children;        /* 组合 */

	/* 图表（kind == Chart）：v1 支持 簇状柱形/条形、饼图、折线图 */
	struct ChartSerie {
		std::wstring name;
		std::vector<double> vals;
	};
	std::wstring chartType;        /* bar / pie / line */
	bool chartHorizontal = false;  /* barDir=bar（水平） */
	std::wstring chartTitle;
	std::vector<std::wstring> chartCats;
	std::vector<ChartSerie> chartSeries;

	/* 表格（kind == Table） */
	std::vector<double> colW;                  /* 列宽（EMU） */
	std::vector<double> rowH;                  /* 行高（EMU） */
	std::vector<std::vector<TableCell>> cells; /* [row][col] */
	bool tblFirstRow = false;                  /* a:tblPr@firstRow（表头带纹） */
	bool tblBandRow = false;                   /* a:tblPr@bandRow（隔行带纹） */
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

/* 文档使用到的字体族（去重、大小写不敏感；忽略空值与 "+mn-lt" 等主题占位符） */
std::vector<std::wstring> HfrDeckCollectFonts(const Deck &deck);

/* 文档使用但本机未安装的字体族（GDI 枚举系统字体，大小写不敏感比较） */
std::vector<std::wstring> HfrDeckMissingFonts(const Deck &deck);

/* 调试用：把 Deck 摘要成文本 */
std::string DeckSummary(const Deck &deck);

} // namespace hfr