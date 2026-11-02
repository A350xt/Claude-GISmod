// 请求级数据解析器：一次分析请求 = 一个分析坐标系。
//
// 坐标系策略：
// - 显式 crs（"source" 跟随首个输入，或任意 EPSG 串）优先级最高；
// - 否则首个进入管线的数据决定：米/英尺单位的投影坐标系直接采用，地理坐标按范围选 UTM；
// - 之后每个输入先修复几何，再重投影到该坐标系并写告警，下游算子看到的永远是同一坐标系。
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "artifact.hpp"
#include "common.hpp"
#include "layer.hpp"
#include "raster.hpp"

namespace gis {

/// 缺失坐标系时只允许一种安全假定：坐标在经纬度值域内 → EPSG:4326（并告警）
void assume_missing_crs(Layer& layer, const std::string& provenance, std::vector<std::string>& warnings);
/// 剔除空几何、修复无效几何（全部留痕）；修不好抛 E_GEOMETRY_INVALID_UNFIXABLE
void repair_geometries(Layer& layer, std::vector<std::string>& warnings);
/// 对图层按范围选 UTM
Srs estimate_utm_for(const Layer& layer);

class Resolver {
public:
    Resolver(ArtifactStore& store, std::optional<std::string> crs_override);

    ArtifactStore& store;
    std::vector<std::string> warnings;
    json inputs_meta = json::array();

    void register_step(const std::string& step_id, const Data& data);
    std::shared_ptr<Layer> frame(const std::string& token);
    std::shared_ptr<Raster> raster(const std::string& token);
    /// 只看已缓存的结果判断某个引用是不是栅格（未加载的路径/产物按内容判断）
    bool is_raster_token(const std::string& token);
    const Srs& analysis_crs() const { return analysis_; }
    json crs_info() const;
    std::vector<std::string> unique_warnings() const;

private:
    std::map<std::string, Data> frames_;
    std::map<std::string, json> prov_;
    json crs_sources_ = json::object();
    Srs analysis_;
    std::optional<std::string> override_;

    Data resolve_source(const std::string& token, json& prov);
    void choose_crs(const Layer& layer);
    void note_analysis_crs(bool was_none);
};

}  // namespace gis
