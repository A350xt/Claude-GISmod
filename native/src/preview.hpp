// 面板与对话卡片的地图预览：
// - SVG「地图卡片」：坐标格网与刻度、比例尺、指北针、坐标系徽标，内嵌亮/暗两套配色，≤ 120 KB；
// - PNG：终端（kitty / iTerm2 图片协议）用，经 GDAL 的 PNG 驱动编码。
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "layer.hpp"
#include "raster.hpp"

namespace gis {

inline constexpr size_t SVG_BUDGET = 120'000;  // 面板 Svg 元素上限 131072 字符，留余量

/// "source"/空 = 源坐标系；"utm" = 按范围选 UTM；其余按 EPSG/WKT 解析
Srs resolve_display_crs(const Srs& source, const std::string& request, const Bounds& bounds);

std::string vector_svg(const Layer& layer, const std::string& title);
/// 栅格卡片：返回 (SVG, 内嵌用的 PNG 字节)
std::pair<std::string, std::vector<unsigned char>> raster_svg(const Raster& rf, const std::string& title);
std::vector<unsigned char> vector_png(const Layer& layer, int width_px = 560);

/// 显示用重投影：图层 / 栅格
Layer reproject_layer(const Layer& layer, const Srs& dst);

}  // namespace gis
