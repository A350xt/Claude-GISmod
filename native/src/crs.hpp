// 坐标系：解析、比较、命名、按范围选 UTM。统一使用"经度在前"的传统 GIS 轴序。
#pragma once

#include <array>
#include <memory>
#include <string>

#include <ogr_spatialref.h>

#include "common.hpp"

namespace gis {

using Srs = std::shared_ptr<OGRSpatialReference>;
using Bounds = std::array<double, 4>;  // minx, miny, maxx, maxy

/// "EPSG:4326" / WKT / PROJ 串 → Srs；无法识别时抛 E_BAD_REQUEST（message 里带 what）
Srs srs_from_user(const std::string& text, const std::string& what = "crs");
Srs srs_clone(const OGRSpatialReference* srs);
Srs srs_epsg(int code);

/// 与 pyproj CRS.to_string() 对齐：能识别出权威代码时给 "EPSG:xxxx"
std::string srs_string(const Srs& srs);
/// 面板徽标：「EPSG:4326 · WGS 84」
std::string srs_label(const Srs& srs);
std::string srs_name(const Srs& srs);
int srs_epsg_code(const Srs& srs);  // 0 = 无
bool srs_same(const Srs& a, const Srs& b);
bool srs_geographic(const Srs& srs);
bool srs_projected(const Srs& srs);
/// 投影坐标系的长度单位名（"metre" / "US survey foot"…），地理坐标系返回 "degree"
std::string srs_unit_name(const Srs& srs);
/// 一个坐标单位等于多少米（地理坐标系不适用，返回 0）
double srs_unit_meters(const Srs& srs);

/// 按数据范围中心选 WGS84 UTM 带（与 geopandas estimate_utm_crs 一致的结果）
Srs estimate_utm(const Bounds& bounds, const Srs& src);

/// 把一个范围变换到另一个坐标系（取 21×21 采样点的外包）
Bounds transform_bounds(const Bounds& b, const Srs& from, const Srs& to);

}  // namespace gis
