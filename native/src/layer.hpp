// 矢量数据模型：带类型的属性表 + 几何列 + 坐标系；以及读写（OGR）。
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <ogr_geometry.h>

#include "common.hpp"
#include "crs.hpp"

namespace gis {

// ---------------------------------------------------------------- 值
enum class FieldType { Integer, Real, String, DateTime, Boolean };

/// DateTime 列存「本地无时区」的 epoch 秒（double），序列化为 ISO 文本。
using Value = std::variant<std::monostate, bool, std::int64_t, double, std::string>;

inline bool is_null(const Value& v) {
    if (std::holds_alternative<std::monostate>(v)) return true;
    if (auto d = std::get_if<double>(&v)) return std::isnan(*d);
    return false;
}
/// 数值视图：整数/实数/布尔/日期（epoch 秒）；其余 NaN
double as_double(const Value& v);
std::string as_text(const Value& v, FieldType type);
json to_json(const Value& v, FieldType type);
/// 用于分组/去重的稳定键
std::string value_key(const Value& v);

/// ISO 8601（YYYY-MM-DD[ T]HH:MM[:SS[.fff]][Z|±HH:MM]）→ epoch 秒；失败返回 nullopt
std::optional<double> parse_datetime(const std::string& text);
std::string format_datetime(double epoch);

// ---------------------------------------------------------------- 几何
using GeomPtr = std::shared_ptr<OGRGeometry>;
GeomPtr own(OGRGeometry* g);
GeomPtr clone_geom(const OGRGeometry* g);

enum class Family { Point, Line, Polygon, Mixed, Empty };
Family family_of(const std::string& geom_type);
std::string geom_type_name(const OGRGeometry* g);  // "Point" / "MultiPolygon"…（shapely 命名）
double geom_area(const OGRGeometry* g);
double geom_length(const OGRGeometry* g);
GeomPtr unary_union(const std::vector<GeomPtr>& geoms);
/// 只保留与 dim（0 点 / 1 线 / 2 面）同维的部分；没有则返回空指针
GeomPtr keep_dimension(GeomPtr g, int dim);
int geom_dimension(const OGRGeometry* g);

// ---------------------------------------------------------------- 图层
struct FieldDef {
    std::string name;
    FieldType type;
};

struct Layer {
    std::vector<FieldDef> fields;
    std::vector<std::vector<Value>> rows;
    std::vector<GeomPtr> geoms;
    Srs srs;

    size_t size() const { return geoms.size(); }
    bool empty() const { return geoms.empty(); }
    int field_index(const std::string& name) const;
    /// 新增列（已有同名列时返回其下标并改类型）；已有行补空值
    int add_field(const std::string& name, FieldType type);
    std::vector<std::string> column_names() const;  // 含 "geometry"，与 geopandas 一致
    Layer empty_like() const;
    void push(std::vector<Value> row, GeomPtr geom);
    Bounds bounds() const;
    std::vector<std::string> geom_types() const;  // 排序去重
    Family family() const;
    Layer select(const std::vector<size_t>& idx) const;
};

struct Provenance {
    std::string format;
    std::string path;
    std::string layer;
};

/// 读矢量数据集：GeoJSON / GeoPackage / Shapefile / 带经纬度列的 CSV。
Layer read_vector(const std::string& path, const std::string& layer_name, Provenance& prov);
/// GPKG 里所有图层名
std::vector<std::string> list_layers(const std::string& path);

/// 写出：driver = "GPKG" / "GeoJSON"；append=true 时往已有 GPKG 追加图层
void write_vector(const Layer& layer, const fs::path& path, const std::string& layer_name,
                  const std::string& driver, bool append);

/// 预览表（前 limit 行、最多 12 个非几何列）
json preview_table(const Layer& layer, int limit = 10);

}  // namespace gis
