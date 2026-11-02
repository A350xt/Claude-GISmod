#include "resolver.hpp"

#include <algorithm>
#include <set>

#include <cpl_error.h>
#include <ogr_geometry.h>

namespace gis {

// ================================================================ 数据准备

void assume_missing_crs(Layer& layer, const std::string& provenance, std::vector<std::string>& warnings) {
    if (layer.srs) return;
    const Bounds b = layer.bounds();
    const bool in_range = b[0] >= -181 && b[0] <= 181 && b[1] >= -91 && b[1] <= 91 && b[2] >= -181 && b[2] <= 181 &&
                          b[3] >= -91 && b[3] <= 91;
    if (!in_range) {
        throw GisError(E_CRS_AMBIGUOUS, "数据缺失 CRS 且坐标值不在经纬度范围内，无法安全假定坐标系：" + provenance,
                       json{{"bounds", {num(b[0]), num(b[1]), num(b[2]), num(b[3])}}});
    }
    warnings.push_back(provenance + " 缺失 CRS：坐标在经纬度范围内，已按 EPSG:4326 处理");
    layer.srs = srs_epsg(4326);
}

namespace {

/// 无效原因：GEOS 通过 GDAL 的错误通道报告（如 "Self-intersection[1 1]"）
std::string explain_invalid(const OGRGeometry* g) {
    CPLErrorReset();
    CPLPushErrorHandler(CPLQuietErrorHandler);
    g->IsValid();
    std::string msg = CPLGetLastErrorMsg();
    CPLPopErrorHandler();
    CPLErrorReset();
    return msg.empty() ? "Invalid Geometry" : msg;
}

bool quiet_valid(const OGRGeometry* g) {
    CPLPushErrorHandler(CPLQuietErrorHandler);
    const bool ok = g->IsValid();
    CPLPopErrorHandler();
    CPLErrorReset();
    return ok;
}

/// make_valid 可能产出 GeometryCollection：按 面 → 线 → 点 取最高维的一组作代表几何
GeomPtr simplify_collection(GeomPtr g) {
    if (!g || wkbFlatten(g->getGeometryType()) != wkbGeometryCollection) return g;
    for (int dim : {2, 1, 0}) {
        std::vector<GeomPtr> group;
        for (const auto* part : *g->toGeometryCollection()) {
            if (!part->IsEmpty() && part->getDimension() == dim) group.push_back(clone_geom(part));
        }
        if (group.size() == 1) return group[0];
        if (!group.empty()) return unary_union(group);
    }
    return g;
}

}  // namespace

void repair_geometries(Layer& layer, std::vector<std::string>& warnings) {
    if (layer.empty()) return;
    std::vector<size_t> keep;
    for (size_t i = 0; i < layer.size(); ++i)
        if (layer.geoms[i] && !layer.geoms[i]->IsEmpty()) keep.push_back(i);
    if (keep.size() != layer.size()) {
        warnings.push_back(std::to_string(layer.size() - keep.size()) + " 条要素无几何或几何为空，已剔除");
        layer = layer.select(keep);
    }
    if (layer.empty()) return;

    std::set<std::string> reasons;
    size_t n_invalid = 0, n_bad = 0;
    for (auto& g : layer.geoms) {
        if (quiet_valid(g.get())) continue;
        ++n_invalid;
        if (reasons.size() < 5) reasons.insert(explain_invalid(g.get()));
        GeomPtr fixed = simplify_collection(own(g->MakeValid()));
        if (!fixed || fixed->IsEmpty() || !quiet_valid(fixed.get())) {
            ++n_bad;
            continue;
        }
        g = fixed;
    }
    if (n_bad) {
        throw GisError(E_GEOMETRY_INVALID_UNFIXABLE, std::to_string(n_bad) + " 条要素几何无法修复",
                       json{{"samples", std::vector<std::string>(reasons.begin(), reasons.end())}});
    }
    if (n_invalid) {
        warnings.push_back("自动修复了 " + std::to_string(n_invalid) + " 个无效几何（" +
                           join(std::vector<std::string>(reasons.begin(), reasons.end()), "；") + "）");
    }
}

Srs estimate_utm_for(const Layer& layer) { return estimate_utm(layer.bounds(), layer.srs); }

// ================================================================ 解析器

Resolver::Resolver(ArtifactStore& s, std::optional<std::string> crs_override) : store(s), override_(std::move(crs_override)) {}

void Resolver::register_step(const std::string& step_id, const Data& data) {
    const std::string key = "@" + step_id;
    frames_[key] = data;
    prov_[key] = {{"kind", data.is_raster() ? "raster" : "step"},
                  {"step", step_id},
                  {"crs", data.is_raster() ? srs_string(data.ras->srs) : srs_string(data.vec->srs)}};
}

Data Resolver::resolve_source(const std::string& token, json& prov) {
    if (is_artifact_token(token)) {
        const fs::path path = store.resolve_result_path(token);
        prov["artifact"] = token;
        if (is_raster_path(u8str(path))) {
            prov["format"] = "raster";
            prov["path"] = native_str(path);
            return Data{nullptr, std::make_shared<Raster>(load_raster(u8str(path)))};
        }
        Provenance p;
        auto layer = std::make_shared<Layer>(read_vector(u8str(path), "", p));
        prov["format"] = p.format;
        prov["path"] = p.path;
        prov["layer"] = p.layer.empty() ? json(nullptr) : json(p.layer);
        return Data{layer, nullptr};
    }
    if (auto ref = parse_step_ref(token)) {
        const auto& [art_id, step_id] = *ref;
        const json meta = store.get(art_id);
        const json steps = meta.value("intermediate_layers", json::object());
        if (!steps.contains(step_id)) {
            std::vector<std::string> available;
            for (auto it = steps.begin(); it != steps.end(); ++it) available.push_back(it.key());
            std::sort(available.begin(), available.end());
            throw GisError(E_INPUT_MISSING, "artifact " + art_id + " 没有中间步骤 " + step_id,
                           json{{"available_steps", available}});
        }
        const std::string layer_name = steps[step_id].value("layer", "");
        const fs::path result_path = store.resolve_result_path(art_id);
        prov["artifact"] = art_id;
        prov["step"] = step_id;
        if (ends_with_ci(layer_name, ".tif")) {
            const fs::path tif = result_path.parent_path() / u8path(layer_name);
            prov["format"] = "raster";
            prov["path"] = native_str(tif);
            return Data{nullptr, std::make_shared<Raster>(load_raster(u8str(tif)))};
        }
        Provenance p;
        auto layer = std::make_shared<Layer>(read_vector(u8str(result_path), layer_name, p));
        prov["format"] = p.format;
        prov["path"] = p.path;
        prov["layer"] = p.layer;
        return Data{layer, nullptr};
    }
    const fs::path path = u8path(token);
    if (!path.is_absolute()) {
        throw GisError(E_INPUT_MISSING, "输入不存在：" + native_str(path), json{{"token", token}});
    }
    if (is_raster_path(token)) {
        prov["format"] = "raster";
        prov["path"] = native_str(path);
        return Data{nullptr, std::make_shared<Raster>(load_raster(token))};
    }
    Provenance p;
    auto layer = std::make_shared<Layer>(read_vector(token, "", p));
    prov["format"] = p.format;
    prov["path"] = p.path;
    prov["layer"] = p.layer.empty() ? json(nullptr) : json(p.layer);
    return Data{layer, nullptr};
}

void Resolver::choose_crs(const Layer& layer) {
    if (analysis_) return;
    const Srs& src = layer.srs;
    if (override_) {
        const std::string o = lower(*override_);
        if (o == "source" || o == "keep" || o == "keep_source") {
            // 保持首个输入的坐标系；缺失 CRS 时仍回退 UTM（无法度量）
            analysis_ = src ? src : estimate_utm_for(layer);
            return;
        }
        analysis_ = srs_from_user(*override_, "crs");
        return;
    }
    if (srs_projected(src)) {
        const std::string unit = lower(srs_unit_name(src));
        if (unit.find("metre") != std::string::npos || unit.find("meter") != std::string::npos ||
            unit.find("foot") != std::string::npos) {
            analysis_ = src;
        } else {
            analysis_ = estimate_utm_for(layer);
        }
        return;
    }
    analysis_ = estimate_utm_for(layer);
}

void Resolver::note_analysis_crs(bool was_none) {
    if (was_none && analysis_) warnings.insert(warnings.begin(), "本次分析的统一坐标系：" + srs_string(analysis_));
}

std::shared_ptr<Layer> Resolver::frame(const std::string& key) {
    if (auto it = frames_.find(key); it != frames_.end()) {
        if (it->second.is_raster()) {
            throw GisError(E_BAD_REQUEST, key + " 是栅格图层；矢量算子需要矢量输入（可用 raster.* 算子处理它）",
                           json{{"token", key}});
        }
        return it->second.vec;
    }
    json prov = json::object();
    Data d = resolve_source(key, prov);
    if (d.is_raster()) {
        throw GisError(E_BAD_REQUEST, key + " 是栅格数据；矢量算子需要矢量输入（先 raster.polygonize/sample 转矢量）",
                       json{{"token", key}});
    }
    auto layer = std::make_shared<Layer>(*d.vec);
    const std::string fmt = prov.value("format", "input");
    const std::string name = fmt + ":" + key;
    assume_missing_crs(*layer, fmt, warnings);
    repair_geometries(*layer, warnings);

    const bool was_none = !analysis_;
    const std::string origin = layer->srs ? srs_string(layer->srs) : "<无CRS>";
    choose_crs(*layer);
    crs_sources_[name] = origin;
    if (analysis_ && !srs_same(layer->srs, analysis_)) {
        warnings.push_back(name + " 已从 " + origin + " 统一重投影到分析坐标系 " + srs_string(analysis_));
        std::unique_ptr<OGRCoordinateTransformation> ct(OGRCreateCoordinateTransformation(layer->srs.get(), analysis_.get()));
        if (!ct) throw GisError(E_BAD_REQUEST, "无法建立坐标变换：" + origin + " → " + srs_string(analysis_));
        for (auto& g : layer->geoms) {
            if (!g) continue;
            GeomPtr moved = clone_geom(g.get());
            if (moved->transform(ct.get()) != OGRERR_NONE)
                throw GisError(E_BAD_REQUEST, name + " 重投影失败（坐标超出目标坐标系定义域？）");
            g = moved;
        }
        layer->srs = analysis_;
    }
    note_analysis_crs(was_none);

    json out = {{"ref", key},
                {"format", prov.value("format", json(nullptr))},
                {"path", prov.value("path", json(nullptr))},
                {"layer", prov.value("layer", json(nullptr))},
                {"count", layer->size()}};
    if (prov.contains("artifact")) out["artifact"] = prov["artifact"];
    if (prov.contains("step")) out["step"] = prov["step"];
    inputs_meta.push_back(out);
    frames_[key] = Data{layer, nullptr};
    prov_[key] = out;
    return layer;
}

std::shared_ptr<Raster> Resolver::raster(const std::string& key) {
    if (auto it = frames_.find(key); it != frames_.end()) {
        if (!it->second.is_raster()) {
            throw GisError(E_BAD_REQUEST, key + " 是矢量图层；栅格算子需要栅格输入（先 raster.from_vector 栅格化）",
                           json{{"token", key}});
        }
        return it->second.ras;
    }
    json prov = json::object();
    Data d = resolve_source(key, prov);
    if (!d.is_raster()) {
        throw GisError(E_BAD_REQUEST, key + " 是矢量数据；栅格算子需要栅格输入（先 raster.from_vector 栅格化）",
                       json{{"token", key}});
    }
    const std::string name = "raster:" + key;
    const bool was_none = !analysis_;
    const std::string origin = d.ras->srs ? srs_string(d.ras->srs) : "None";
    if (!analysis_) {
        const Srs& src = d.ras->srs;
        if (override_) {
            const std::string o = lower(*override_);
            analysis_ = (o == "source" || o == "keep" || o == "keep_source")
                            ? (src ? src : estimate_utm(d.ras->bounds(), src))
                            : srs_from_user(*override_, "crs");
        } else if (srs_projected(src)) {
            const std::string unit = lower(srs_unit_name(src));
            analysis_ = (unit.find("metre") != std::string::npos || unit.find("meter") != std::string::npos ||
                         unit.find("foot") != std::string::npos)
                            ? src
                            : estimate_utm(d.ras->bounds(), src);
        } else {
            analysis_ = estimate_utm(d.ras->bounds(), src ? src : srs_epsg(4326));
        }
    }
    crs_sources_[name] = origin;
    auto rf = std::make_shared<Raster>(*d.ras);
    if (!rf->srs) {
        rf->srs = analysis_;
    } else if (!srs_same(rf->srs, analysis_)) {
        warnings.push_back(name + " 已从 " + origin + " 统一重投影到分析坐标系 " + srs_string(analysis_));
        *rf = reproject_raster(*rf, analysis_);
    }
    note_analysis_crs(was_none);

    json out = {{"ref", key}, {"format", "raster"}, {"path", prov.value("path", json(nullptr))}, {"count", rf->cells()}};
    if (prov.contains("artifact")) out["artifact"] = prov["artifact"];
    if (prov.contains("step")) out["step"] = prov["step"];
    inputs_meta.push_back(out);
    frames_[key] = Data{nullptr, rf};
    prov_[key] = out;
    return rf;
}

bool Resolver::is_raster_token(const std::string& token) {
    if (auto it = frames_.find(token); it != frames_.end()) return it->second.is_raster();
    if (is_raster_path(token)) return true;
    if (is_artifact_token(token)) {
        const json meta = store.get(token);
        return lower(meta.value("result", json::object()).value("format", "")) == "tif";
    }
    if (auto ref = parse_step_ref(token)) {
        const json meta = store.get(ref->first);
        const json info = meta.value("intermediate_layers", json::object()).value(ref->second, json::object());
        return info.value("kind", "") == "raster" || ends_with_ci(info.value("layer", ""), ".tif");
    }
    return false;
}

json Resolver::crs_info() const {
    return {{"analysis", analysis_ ? srs_string(analysis_) : ""}, {"sources", crs_sources_}};
}

std::vector<std::string> Resolver::unique_warnings() const {
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const auto& w : warnings)
        if (seen.insert(w).second) out.push_back(w);
    return out;
}

}  // namespace gis
