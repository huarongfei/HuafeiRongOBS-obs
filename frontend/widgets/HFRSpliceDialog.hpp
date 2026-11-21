#pragma once

#include <QDialog>

#include <QVector>
#include <functional>
#include <vector>

#include <obs.hpp>

class QTableWidget;
class QLabel;

/* 拼接块：把画布（=拼接墙，自定义分辨率）上的一块矩形区域，铺满投到指定编号的物理屏。
 * 画布分辨率即"墙"的逻辑分辨率：任意尺寸（如 3840x1080 / 1080x1920 / 5000x1200），
 * 每块 tile 用像素坐标 (x,y,w,h) 指定该屏显示墙的哪一部分。 */
struct HfrWallTile {
	int screen = -1;
	int x = 0, y = 0, w = 0, h = 0;
	int rot = 0; /* 0/90/180/270：该物理屏相对墙的旋转（竖装/倒装屏） */
	/* 边缘融合：该块四边各留多宽的衰减带（墙像素），与相邻块互补实现无缝拼接（旋转块暂不融合） */
	int blendL = 0, blendR = 0, blendT = 0, blendB = 0;
	/* 四角几何校正（keystone）：各角在墙像素坐标下的位移（正 X 右 / 正 Y 下） */
	int kTLx = 0, kTLy = 0, kTRx = 0, kTRy = 0;
	int kBLx = 0, kBLy = 0, kBRx = 0, kBRy = 0;
	/* 色彩统一（多投影机色差校正）：输出 = pow(saturate(in*gain + lift), 1/gamma) */
	double gammaV = 1.0;
	double liftV = 0.0;
	double gainV = 1.0;
	bool HasKeystone() const
	{
		return kTLx || kTLy || kTRx || kTRy || kBLx || kBLy || kBRx || kBRy;
	}
	bool HasColorAdjust() const
	{
		return gammaV != 1.0 || liftV != 0.0 || gainV != 1.0;
	}
	/* 曲面网格校正：3×3 控制点中的 5 个非角点偏移（墙像素，正右/正下） */
	int cMTx = 0, cMTy = 0; /* 上中 */
	int cMRx = 0, cMRy = 0; /* 右中 */
	int cMBx = 0, cMBy = 0; /* 下中 */
	int cMLx = 0, cMLy = 0; /* 左中 */
	int cCx = 0, cCy = 0;   /* 中心 */
	bool HasCurve() const
	{
		return cMTx || cMTy || cMRx || cMRy || cMBx || cMBy || cMLx || cMLy || cCx || cCy;
	}
};

class HFRSpliceDialog : public QDialog {
public:
	using ApplyFn = std::function<void(obs_canvas_t *canvas, const QVector<HfrWallTile> &tiles)>;

	/* initial：上次保存的拼接块（可空，自动生成等分默认值） */
	HFRSpliceDialog(obs_canvas_t *canvas, const QVector<HfrWallTile> &initial, ApplyFn onApply,
			QWidget *parent = nullptr);

private:
	void RebuildDefaultTiles();
	void AddRow(const HfrWallTile &t);
	void RemoveSelectedRow(); 
	std::vector<HfrWallTile> CollectTiles() const;

	/* 行的完整数据（隐藏字段：曲面网格偏移等由子对话框编辑） */
	QVector<HfrWallTile> rowData;

	obs_canvas_t *canvas = nullptr;
	ApplyFn applyFn;
	QTableWidget *table = nullptr;
	QLabel *infoLabel = nullptr;
	int canvasW = 1920, canvasH = 1080;
};
