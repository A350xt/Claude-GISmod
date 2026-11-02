// 栅格基建：单波段 float32 + NaN=NoData；读写 GeoTIFF、对齐、重投影、矢量栅格化。
//
// 三条约定（与矢量域一致）：
// 1. 组合类算子默认对齐首个输入的模板，不一致时自动重采样并写 warning；
// 2. 每个栅格算子的摘要自带元信息、NoData 占比与分位数；
// 3. NoData 一律用 NaN 表达，落盘为 GeoTIFF 的 NaN nodata。
#pragma once

#include <array>
#include <string>
#include <utility>
#include <vector>

#include "common.hpp"
#include "crs.hpp"
#include "layer.hpp"

namespace gis {

inline constexpr size_t MAX_CELLS = 4'000'000;  // 单幅栅格像元上限，防失控

using GeoTransform = std::array<double, 6>;

struct Raster {
    int width = 0, height = 0;
    std::vector<float> data;  // 行优先
    GeoTransform gt{0, 1, 0, 0, 0, -1};
    Srs srs;
    std::string name = "raster";

    float& at(int r, int c) { return data[static_cast<size_t>(r) * width + c]; }
    float at(int r, int c) const { return data[static_cast<size_t>(r) * width + c]; }
    size_t cells() const { return data.size(); }
    double cellsize() const { return std::fabs(gt[1]); }
    Bounds bounds() const;
    size_t valid_cells() const;
    bool all_nodata() const { return valid_cells() == 0; }
    /// 世界坐标 → 像元坐标（浮点列、行）
    std::pair<double, double> to_pixel(double x, double y) const;
    /// 像元中心 → 世界坐标
    std::pair<double, double> center_of(int r, int c) const;

    json meta() const;
    std::vector<std::string> summaries() const;
    json table() const;
};

Raster blank_raster(int height, int width, const GeoTransform& gt, const Srs& srs, float fill = static_cast<float>(NaN),
                    const std::string& name = "raster");
void check_cells(const Raster& rf);

bool is_raster_path(const std::string& path);
Raster load_raster(const std::string& path, int band = 1);
json raster_meta_of(const std::string& path);
void save_raster(const Raster& rf, const fs::path& path);

bool same_grid(const Raster& a, const Raster& b);
/// 重采样到模板网格（method: nearest / bilinear / cubic）
Raster align_to(const Raster& rf, const Raster& tmpl, const std::string& method = "nearest");
/// 重投影到另一坐标系（网格由 GDAL 推荐）
Raster reproject_raster(const Raster& rf, const Srs& dst);

/// bounds + cellsize → 左上角原点的仿射与 (height, width)；snap 时原点贴 cellsize 整数倍
std::pair<GeoTransform, std::pair<int, int>> make_transform(const Bounds& bounds, double cellsize, bool snap = true);

/// 矢量 → 栅格（field 为空时覆盖处 = default_value；字段值为空的要素跳过）
std::vector<float> rasterize(const Layer& layer, const GeoTransform& gt, int height, int width,
                             const std::string& field = "", double default_value = 1.0, bool all_touched = false);
/// 按给定的每要素值栅格化
std::vector<float> rasterize_values(const std::vector<GeomPtr>& geoms, const std::vector<double>& values,
                                    const GeoTransform& gt, int height, int width, bool all_touched = false);

/// 分位数（numpy 默认的线性插值）；values 会被排序
double quantile_sorted(const std::vector<float>& sorted, double q);
std::vector<float> finite_values(const std::vector<float>& data);

}  // namespace gis
