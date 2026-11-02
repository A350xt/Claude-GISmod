#include "api.hpp"

#include <algorithm>
#include <fstream>
#include <regex>
#include <set>

#include <cpl_error.h>

#include "artifact.hpp"
#include "ops.hpp"
#include "preview.hpp"
#include "resolver.hpp"

namespace gis {

namespace {

std::string pandas_dtype(FieldType t) {
    switch (t) {
        case FieldType::Integer: return "int64";
        case FieldType::Real: return "float64";
        case FieldType::DateTime: return "datetime64[ms]";
        case FieldType::Boolean: return "bool";
        default: return "object";
    }
}

bool quiet_valid(const OGRGeometry* g) {
    CPLPushErrorHandler(CPLQuietErrorHandler);
    const bool ok = g->IsValid();
    CPLPopErrorHandler();
    CPLErrorReset();
    return ok;
}

}  // namespace

// ================================================================ inspect

json action_inspect(const fs::path&, const json& payload) {
    const std::string path = payload.value("path", "");
    if (is_raster_path(path)) {
        json meta = raster_meta_of(path);
        meta["source"] = {{"format", "tif"}, {"path", path}};
        meta["capabilities"] = {"raster.*"};
        meta["supported_formats_note"] = "栅格已支持（raster 域算子）；多波段先指定 band";
        meta["notes"] = {"NoData 占比 " + py_float(meta["nodata_pct"].get<double>()) + "%",
                         "栅格化/重采样/统计请走 analyze 工具的 raster.* 词表"};
        return meta;
    }
    Provenance prov;
    const Layer layer = read_vector(path, payload.value("layer", ""), prov);

    json types = json::object();
    size_t n_invalid = 0, n_null = 0;
    for (const auto& g : layer.geoms) {
        if (!g) {
            ++n_null;
            continue;
        }
        const std::string t = geom_type_name(g.get());
        types[t] = types.value(t, 0) + 1;
        if (!quiet_valid(g.get())) ++n_invalid;
    }
    const Bounds b = layer.bounds();
    json fields = json::array();
    for (size_t i = 0; i < layer.fields.size(); ++i) {
        json samples = json::array();
        std::set<std::string> seen;
        for (const auto& row : layer.rows) {
            if (samples.size() >= 3) break;
            if (is_null(row[i])) continue;
            if (!seen.insert(value_key(row[i])).second) continue;
            samples.push_back(to_json(row[i], layer.fields[i].type));
        }
        fields.push_back({{"name", layer.fields[i].name}, {"dtype", pandas_dtype(layer.fields[i].type)}, {"samples", samples}});
    }
    const bool in_range = b[0] >= -181 && b[0] <= 181 && b[1] >= -91 && b[1] <= 91 && b[2] >= -181 && b[2] <= 181 &&
                          b[3] >= -91 && b[3] <= 91;
    json notes = json::array();
    if (!layer.srs)
        notes.push_back(std::string("数据缺失 CRS") + (in_range ? "（坐标在经纬度范围内，分析时可安全假定 EPSG:4326）"
                                                                : "且坐标不在经纬度范围内，度量前必须先明确 CRS"));
    if (n_invalid) notes.push_back(std::to_string(n_invalid) + " 个无效几何，分析时将自动修复");
    if (n_null) notes.push_back(std::to_string(n_null) + " 条要素缺少几何");
    json source = {{"format", prov.format}, {"path", prov.path}};
    if (!prov.layer.empty()) source["layer"] = prov.layer;
    return {
        {"kind", "vector"},
        {"source", source},
        {"geometry_types", types},
        {"count", layer.size()},
        {"crs", layer.srs ? json(srs_string(layer.srs)) : json(nullptr)},
        {"extent_bbox", {num(b[0]), num(b[1]), num(b[2]), num(b[3])}},
        {"fields", fields},
        {"quality",
         {{"invalid_geometries", n_invalid},
          {"null_geometries", n_null},
          {"missing_crs", !layer.srs},
          {"wgs84_assumable_if_missing", in_range}}},
        {"capabilities", {"query_measure", "proximity", "overlay", "zonal", "raster"}},
        {"supported_formats_note", "矢量 + 栅格（raster.*）；网络分析与空间统计属于后续阶段"},
        {"notes", notes},
    };
}

// ================================================================ analyze

json action_analyze(const fs::path& workdir, const json& request) {
    const json operations = request.value("operations", json(nullptr));
    if (!operations.is_array() || operations.empty())
        throw GisError(E_BAD_REQUEST, "analyze 需要 operations 列表（至少一步）");
    const std::string title = request.contains("title") && request["title"].is_string() && !request["title"].get<std::string>().empty()
                                  ? request["title"].get<std::string>()
                                  : "空间分析";
    const std::string output_format = lower(request.value("output", json::object()).value("format", "gpkg"));

    // 请求级坐标系：显式指定 > 自动策略（纯非度量管线保持源坐标系）
    std::optional<std::string> crs_override;
    if (request.contains("crs") && !request["crs"].is_null()) {
        if (!request["crs"].is_string()) throw GisError(E_BAD_REQUEST, "crs 必须是字符串（如 'EPSG:32622' 或 'source'）");
        crs_override = request["crs"].get<std::string>();
    } else {
        static const std::set<std::string> NON_METRIC = {"query.filter", "query.select", "trajectory.build"};
        bool all_non_metric = true;
        for (const auto& op : operations)
            all_non_metric = all_non_metric && op.is_object() && NON_METRIC.count(op.value("op", ""));
        if (all_non_metric) crs_override = "source";
    }

    ArtifactStore store(workdir);
    const std::string artifact_id = store.create("analysis", title);
    Resolver resolver(store, crs_override);
    const auto& handlers = op_handlers();
    static const std::regex STEP_ID(R"(^[A-Za-z0-9_]{1,32}$)");

    json steps = json::array();
    std::vector<std::string> summaries;
    std::set<std::string> seen_ids;
    std::vector<std::pair<std::string, Data>> intermediates;
    Data final_data;

    for (size_t i = 0; i < operations.size(); ++i) {
        const json& op_req = operations[i];
        if (!op_req.is_object()) throw GisError(E_BAD_REQUEST, "第 " + std::to_string(i + 1) + " 步不是对象");
        const std::string op = op_req.value("op", "");
        if (op.empty()) throw GisError(E_BAD_REQUEST, "第 " + std::to_string(i + 1) + " 步缺少 op 字段");
        const auto handler = handlers.find(op);
        if (handler == handlers.end()) {
            json available = json::array();
            for (const auto& [name, _] : handlers) available.push_back(name);
            throw GisError(E_OP_UNKNOWN, "未知操作：" + op, json{{"step", i + 1}, {"available", available}});
        }
        const std::string raw_id = op_req.contains("id") && !op_req["id"].is_null() ? to_str(op_req["id"]) : "s" + std::to_string(i + 1);
        if (!std::regex_match(raw_id, STEP_ID) || !seen_ids.insert(raw_id).second)
            throw GisError(E_BAD_REQUEST, "步骤 id 非法或重复：" + raw_id, json{{"step", i + 1}});

        json params = json::object();
        for (auto it = op_req.begin(); it != op_req.end(); ++it)
            if (it.key() != "op" && it.key() != "id" && it.key() != "allow_empty") params[it.key()] = it.value();
        const bool allow_empty = op_req.contains("allow_empty") && to_bool(op_req["allow_empty"]);

        StepResult res;
        try {
            res = handler->second(resolver, params, raw_id);
        } catch (GisError& exc) {
            exc.details()["step"] = raw_id;
            exc.details()["op"] = op;
            throw;
        }
        size_t count;
        bool empty;
        if (res.ras) {
            check_cells(*res.ras);
            count = res.ras->valid_cells();
            empty = count == 0;
        } else {
            count = res.vec ? res.vec->size() : 0;
            empty = count == 0;
        }
        if (empty) {
            if (!allow_empty) {
                throw GisError(E_EMPTY_RESULT,
                               "步骤 " + raw_id + "（" + op + "）结果为空；如空集属预期，请在该步加 \"allow_empty\": true",
                               json{{"step", raw_id}, {"op", op}});
            }
            res.summaries.push_back("（步骤 " + raw_id + " 结果为空——已按 allow_empty 继续）");
        }
        const Data data{res.ras ? nullptr : res.vec, res.ras};
        resolver.register_step(raw_id, data);
        intermediates.emplace_back(raw_id, data);
        json entry = {{"id", raw_id}, {"op", op}, {"count", count}, {"summary", res.summaries.empty() ? "" : res.summaries[0]}};
        if (!res.table.is_null()) entry["table"] = res.table;
        steps.push_back(entry);
        summaries.insert(summaries.end(), res.summaries.begin(), res.summaries.end());
        final_data = data;
    }
    if (!final_data.vec && !final_data.ras) final_data.vec = std::make_shared<Layer>();

    const json meta = store.finalize_analysis(artifact_id, title, operations, steps, summaries, resolver.unique_warnings(),
                                              resolver.crs_info(), resolver.inputs_meta, final_data, output_format,
                                              intermediates);
    return {
        {"artifact_id", artifact_id},
        {"title", title},
        {"result", meta["result"]},
        {"summary", summaries},
        {"steps", steps},
        {"preview", final_data.is_raster() ? json(nullptr) : preview_table(*final_data.vec)},
        {"warnings", meta["warnings"]},
        {"crs", meta["crs"]},
        {"inputs", resolver.inputs_meta},
        {"intermediate_layers", meta.value("intermediate_layers", json::object())},
    };
}

// ================================================================ show / operations

json action_show(const fs::path& workdir, const json& payload) {
    ArtifactStore store(workdir);
    const std::string id = payload.value("artifact_id", "");
    json meta = store.get(id);
    const auto ids = store.list_ids();
    if (std::find(ids.begin(), ids.end(), meta.value("artifact_id", "")) == ids.end())
        throw GisError(E_INPUT_MISSING, "Artifact 索引异常", json{{"id", id}});
    return meta;
}

json action_operations(const fs::path&, const json&) {
    json ops = json::array();
    for (const auto& [name, _] : op_handlers()) ops.push_back(name);
    return {{"operations", ops}};
}

// ================================================================ list（面板清单）

namespace {

const std::set<std::string> SKIP_DIRS = {"node_modules", ".git", "__pycache__", ".venv", "venv", "dist",
                                         "build",        ".pytest_cache", "artifacts", ".claude", ".claude-plugin"};

bool looks_like_geojson(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::string head(4096, '\0');
    in.read(head.data(), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<size_t>(in.gcount()));
    head = lower(head);
    for (const char* t : {"\"featurecollection\"", "\"features\"", "\"geometries\"", "\"type\": \"feature\""})
        if (head.find(t) != std::string::npos) return true;
    return false;
}

}  // namespace

json action_list(const fs::path& workdir, const json& payload) {
    std::vector<std::string> roots;
    for (const char* key : {"datasets_dirs", "datasets_dir"}) {
        if (!payload.contains(key)) continue;
        const json& v = payload[key];
        if (v.is_string() && !v.get<std::string>().empty()) roots.push_back(v.get<std::string>());
        else if (v.is_array())
            for (const auto& x : v)
                if (x.is_string() && !x.get<std::string>().empty()) roots.push_back(x.get<std::string>());
    }
    json datasets = json::array(), artifacts = json::array();
    size_t skipped = 0;
    std::set<std::string> seen;

    auto push_dataset = [&](const fs::path& p, const std::string& origin) {
        const std::string key = lower(replace_all(u8str(p), "\\", "/"));
        std::error_code ec;
        if (seen.count(key) || !fs::is_regular_file(p, ec)) return;
        const std::string ext = lower(u8str(p.extension()));
        if (ext == ".json" && !looks_like_geojson(p)) return;
        std::string fmt;
        if (ext == ".geojson" || ext == ".json") fmt = "geojson";
        else if (ext == ".gpkg") fmt = "gpkg";
        else if (ext == ".shp") fmt = "shapefile";
        else if (ext == ".csv") fmt = "csv";
        else if (ext == ".tif" || ext == ".tiff") fmt = "tif";
        else return;
        seen.insert(key);
        datasets.push_back({{"name", u8str(p.stem())},
                            {"path", native_str(p)},
                            {"format", fmt},
                            {"size_bytes", fs::file_size(p, ec)},
                            {"origin", origin}});
    };

    ArtifactStore store(workdir);
    const std::string root_key = lower(replace_all(u8str(store.root().parent_path()), "\\", "/"));
    std::vector<json> metas;
    for (const auto& id : store.list_ids()) {
        try {
            metas.push_back(store.get(id));
        } catch (const GisError&) {
            ++skipped;
            continue;
        }
        for (const auto& inp : metas.back().value("inputs", json::array())) {
            const std::string p = inp.value("path", json(nullptr)).is_string() ? inp["path"].get<std::string>() : "";
            if (!p.empty() && !starts_with(lower(replace_all(p, "\\", "/")), root_key)) push_dataset(u8path(p), "agent-used");
        }
    }

    for (const auto& root_str : roots) {
        const fs::path root = u8path(root_str);
        std::error_code ec;
        if (!root.is_absolute() || !fs::is_directory(root, ec)) continue;
        size_t found = 0;
        const size_t limit = 300;
        for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator() && found < limit; it.increment(ec)) {
            if (ec) break;
            const std::string name = u8str(it->path().filename());
            if (it->is_directory(ec)) {
                if (it.depth() >= 3 || SKIP_DIRS.count(name) || starts_with(name, ".")) it.disable_recursion_pending();
                continue;
            }
            const std::string ext = lower(u8str(it->path().extension()));
            if (ext == ".geojson" || ext == ".json" || ext == ".gpkg" || ext == ".shp" || ext == ".csv" || ext == ".tif" ||
                ext == ".tiff") {
                push_dataset(it->path(), "workspace");
                ++found;
            }
        }
    }

    std::vector<json> ds(datasets.begin(), datasets.end());
    std::stable_sort(ds.begin(), ds.end(), [](const json& a, const json& b) {
        auto rank = [](const json& d) { return d["origin"] == "agent-used" ? 0 : d["origin"] == "workspace" ? 1 : 2; };
        if (rank(a) != rank(b)) return rank(a) < rank(b);
        return a["name"].get<std::string>() < b["name"].get<std::string>();
    });

    for (auto it = metas.rbegin(); it != metas.rend(); ++it) {  // 新的在前
        const json& meta = *it;
        const json res = meta.value("result", json::object());
        json images = json::array();
        if (meta.contains("image") && meta["image"].is_string()) images.push_back(meta["image"]);
        for (const auto& img : meta.value("images", json::array()))
            if (img.contains("path")) images.push_back(img["path"]);
        json summary = json::array();
        const json all_summary = meta.value("summary", json::array());
        for (size_t i = 0; i < std::min<size_t>(6, all_summary.size()); ++i) summary.push_back(all_summary[i]);
        artifacts.push_back({{"artifact_id", meta.value("artifact_id", "")},
                             {"kind", meta.value("kind", json(nullptr))},
                             {"title", meta.value("title", "")},
                             {"created", meta.value("created", "")},
                             {"count", res.value("count", 0)},
                             {"geometry_types", res.value("geometry_types", json::array())},
                             {"summary", summary},
                             {"warnings", meta.value("warnings", json::array()).size()},
                             {"steps", meta.value("steps", json::array())},
                             {"intermediate_layers", meta.value("intermediate_layers", json::object())},
                             {"images", images}});
    }
    return {{"datasets", ds}, {"artifacts", artifacts}, {"skipped_corrupt", skipped}};
}

// ================================================================ preview / card

json action_preview(const fs::path& workdir, const json& payload) {
    const std::string source = payload.value("source", "");
    if (source.empty()) throw GisError(E_BAD_REQUEST, "preview 需要 source（路径或 artifact_id）");
    const bool want_png = payload.contains("png") && to_bool(payload["png"]);
    const std::string crs_req = payload.contains("crs") && payload["crs"].is_string() ? payload["crs"].get<std::string>() : "source";
    ArtifactStore store(workdir);
    Resolver resolver(store, std::string("source"));
    const std::string title = starts_with(source, "ar_") ? source : u8str(u8path(source).stem());

    json out;
    Srs source_crs, display;
    std::string svg;
    std::vector<unsigned char> png;
    if (resolver.is_raster_token(source)) {
        auto rf = resolver.raster(source);
        source_crs = rf->srs;
        display = resolve_display_crs(rf->srs, crs_req, rf->bounds());
        Raster shown = display ? reproject_raster(*rf, display) : *rf;
        auto [s, p] = raster_svg(shown, title);
        svg = std::move(s);
        png = std::move(p);
        const json m = shown.meta();
        const Bounds b = shown.bounds();
        out = {{"kind", "raster"},
               {"meta",
                {{"shape", m["shape"]},
                 {"cellsize", m["cellsize"]},
                 {"nodata_pct", m["nodata_pct"]},
                 {"quantiles", m["quantiles"]},
                 {"bounds", {b[0], b[1], b[2], b[3]}}}}};
    } else {
        auto layer = resolver.frame(source);
        source_crs = layer->srs;
        display = resolve_display_crs(layer->srs, crs_req, layer->bounds());
        const Layer shown = display ? reproject_layer(*layer, display) : *layer;
        svg = vector_svg(shown, title);
        if (want_png) png = vector_png(shown);
        json types = json::object();
        for (const auto& g : shown.geoms)
            if (g) types[geom_type_name(g.get())] = types.value(geom_type_name(g.get()), 0) + 1;
        json fields = json::array();
        for (size_t i = 0; i < shown.fields.size() && i < 40; ++i) {
            json samples = json::array();
            for (const auto& row : shown.rows) {
                if (samples.size() >= 2) break;
                if (!is_null(row[i])) samples.push_back(to_json(row[i], shown.fields[i].type));
            }
            fields.push_back({{"name", shown.fields[i].name}, {"dtype", pandas_dtype(shown.fields[i].type)}, {"samples", samples}});
        }
        const Bounds b = shown.bounds();
        out = {{"kind", "vector"},
               {"meta",
                {{"count", shown.size()},
                 {"geometry_types", types},
                 {"bounds", shown.empty() ? json(nullptr) : json{b[0], b[1], b[2], b[3]}},
                 {"fields", fields}}}};
    }
    const Srs& d = display ? display : source_crs;
    out["meta"]["crs"] = d ? json(srs_string(d)) : json(nullptr);
    out["meta"]["crs_label"] = srs_label(d);
    out["meta"]["crs_units"] = d ? (d->IsGeographic() ? std::string("度") : srs_unit_name(d)) : std::string();
    out["meta"]["source_crs"] = source_crs ? json(srs_string(source_crs)) : json(nullptr);
    out["source"] = source;
    out["svg"] = svg;
    out["png_base64"] = (want_png && !png.empty()) ? json(base64(png)) : json(nullptr);
    out["warnings"] = resolver.unique_warnings();
    return out;
}

json action_card(const fs::path& workdir, const std::string& action, const json& request, const json& result, const json& opts) {
    std::string source;
    if (action == "analyze" && result.contains("artifact_id")) source = result["artifact_id"].get<std::string>();
    else if (action == "inspect" && request.contains("path")) source = to_str(request["path"]);
    else return json::object();
    json out = action_preview(workdir, json{{"source", source},
                                            {"crs", opts.value("crs", "source")},
                                            {"png", opts.contains("png") && to_bool(opts["png"])}});
    out.erase("source");
    return out;
}

}  // namespace gis
