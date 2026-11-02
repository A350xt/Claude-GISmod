#include "artifact.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

namespace gis {

namespace {
const std::regex ARTIFACT_ID_RE(R"(^ar_[A-Za-z0-9]{10,32}$)");
const std::regex STEP_REF_RE(R"(^(ar_[A-Za-z0-9]{10,32})#([A-Za-z0-9_]{1,32})$)");
const char* META_NAME = "meta.json";
const char* PLAN_NAME = "plan.json";

std::string new_id() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d%H%M%S", &tm);
    return std::string("ar_") + buf + random_token(4);
}

std::string checked_id(const std::string& id) {
    if (std::regex_match(id, ARTIFACT_ID_RE)) return id;
    throw GisError(E_BAD_REQUEST, "非法 artifact_id：'" + id + "'", json{{"expected_pattern", "^ar_[A-Za-z0-9]{10,32}$"}});
}

bool contained(const fs::path& root, const fs::path& candidate) {
    const std::string r = u8str(root), c = u8str(candidate);
    if (c == r) return true;
    const std::string sep = u8str(fs::path("/").make_preferred());
    return c.size() > r.size() && c.compare(0, r.size(), r) == 0 && (c.substr(r.size(), sep.size()) == sep);
}
}  // namespace

bool is_artifact_token(const std::string& s) { return std::regex_match(s, ARTIFACT_ID_RE); }

std::optional<std::pair<std::string, std::string>> parse_step_ref(const std::string& s) {
    std::smatch m;
    if (std::regex_match(s, m, STEP_REF_RE)) return std::make_pair(m[1].str(), m[2].str());
    return std::nullopt;
}

ArtifactStore::ArtifactStore(const fs::path& workdir) {
    std::error_code ec;
    fs::path base = fs::weakly_canonical(fs::absolute(workdir, ec), ec);
    if (ec) base = fs::absolute(workdir);
    root_ = (base / "artifacts").make_preferred();
    fs::create_directories(root_, ec);
    if (ec) throw GisError(E_OUTPUT_ERROR, "无法创建 artifacts 目录：" + native_str(root_) + "（" + ec.message() + "）");
}

fs::path ArtifactStore::checked(const fs::path& p, const char* what) const {
    std::error_code ec;
    fs::path real = fs::weakly_canonical(p, ec);
    if (ec) real = p;
    real.make_preferred();
    if (!contained(root_, real)) throw GisError(E_OUTPUT_ERROR, std::string(what) + "越界");
    return real;
}

fs::path ArtifactStore::dir_of(const std::string& id) const { return checked(root_ / checked_id(id), "artifact 目录"); }

fs::path ArtifactStore::result_path_of(const std::string& id, const std::string& fmt) const {
    const std::string name = fmt == "geojson" ? "result.geojson" : fmt == "tif" ? "result.tif" : "result.gpkg";
    return checked(dir_of(id) / name, "结果文件");
}

fs::path ArtifactStore::image_path_for(const std::string& id, const std::string& stem) const {
    // 保留中日韩字符，只剔除路径危险字符
    std::string clean;
    for (unsigned char c : stem) {
        if (c >= 0x80 || std::isalnum(c) || c == '_' || c == '-') clean.push_back(static_cast<char>(c));
    }
    if (clean.empty()) clean = "map";
    if (clean.size() > 120) clean.resize(120);
    return checked(dir_of(id) / u8path(clean + ".png"), "图片");
}

void ArtifactStore::write_json(const fs::path& p, const json& payload) const {
    const fs::path target = checked(p, "json 写出");
    fs::path tmp = target;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw GisError(E_OUTPUT_ERROR, "无法写出 " + native_str(target));
        out << payload.dump(2, ' ', false, json::error_handler_t::replace);
    }
    std::error_code ec;
    fs::rename(tmp, target, ec);  // 原子替换：进程被杀不会留下 0 字节 meta
    if (ec) throw GisError(E_OUTPUT_ERROR, "无法写出 " + native_str(target) + "（" + ec.message() + "）");
}

json ArtifactStore::read_json(const fs::path& p) const {
    const fs::path target = checked(p, "json 读取");
    std::ifstream in(target, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    try {
        return json::parse(ss.str());
    } catch (const std::exception& exc) {
        throw GisError(E_OUTPUT_ERROR,
                       "artifact 元数据损坏（" + u8str(target.parent_path().filename()) + "）：" + exc.what(),
                       json{{"file", u8str(target.filename())}});
    }
}

std::string ArtifactStore::create(const std::string& kind, const std::string& title) {
    std::string id = new_id();
    const fs::path dir = dir_of(id);
    fs::create_directories(dir);
    write_json(dir / META_NAME, json{{"artifact_id", id}, {"kind", kind}, {"title", title}, {"created", now_iso()}});
    return id;
}

json ArtifactStore::finalize_analysis(const std::string& id, const std::string& title, const json& operations,
                                      const json& steps, const std::vector<std::string>& summaries,
                                      const std::vector<std::string>& warnings, const json& crs_info, const json& inputs,
                                      const Data& result, const std::string& result_format,
                                      const std::vector<std::pair<std::string, Data>>& intermediates) {
    const fs::path dir = dir_of(id);
    json steps_index = json::object();
    json result_meta;
    try {
        fs::path result_path;
        bool persist_vector_steps = false;
        if (result.is_raster()) {
            result_path = dir / "result.tif";
            save_raster(*result.ras, result_path);
            result_meta = {{"path", native_str(result_path)},
                           {"format", "tif"},
                           {"count", result.ras->valid_cells()},
                           {"geometry_types", {"Raster"}},
                           {"cellsize", result.ras->cellsize()},
                           {"shape", {result.ras->height, result.ras->width}}};
        } else {
            const std::string fmt = result_format == "geojson" ? "geojson" : "gpkg";
            result_path = result_path_of(id, fmt);
            if (fmt == "geojson") {
                write_vector(*result.vec, result_path, "result", "GeoJSON", false);
            } else {
                write_vector(*result.vec, result_path, "result", "GPKG", false);
                persist_vector_steps = true;
            }
            result_meta = {{"path", native_str(result_path)},
                           {"format", fmt},
                           {"count", result.vec->size()},
                           {"geometry_types", result.vec->geom_types()}};
        }
        // 每个步骤都可被 ar_xxx#stepId 引用（含最后一步）
        std::vector<std::pair<std::string, Data>> sorted = intermediates;
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& [sid, val] : sorted) {
            if (val.is_raster()) {
                const fs::path tif = dir / ("step_" + sid + ".tif");
                save_raster(*val.ras, tif);
                steps_index[sid] = {{"layer", u8str(tif.filename())}, {"count", val.ras->valid_cells()}, {"kind", "raster"}};
            } else if (persist_vector_steps && val.vec) {
                const std::string layer = "step_" + sid;
                write_vector(*val.vec, result_path, layer, "GPKG", true);
                steps_index[sid] = {{"layer", layer}, {"count", val.vec->size()}};
            }
        }
    } catch (const GisError&) {
        throw;
    } catch (const std::exception& exc) {
        throw GisError(E_OUTPUT_ERROR, std::string("结果数据集写出失败:") + exc.what());
    }

    write_json(dir / PLAN_NAME,
               json{{"title", title}, {"operations", operations}, {"inputs", inputs}, {"crs", crs_info}, {"steps", steps}});
    json meta = {
        {"artifact_id", id},
        {"kind", "analysis"},
        {"title", title},
        {"created", now_iso()},
        {"result", result_meta},
        {"summary", summaries},
        {"warnings", warnings},
        {"crs", crs_info},
        {"steps", steps},
        {"inputs", inputs},
        {"plan_ref", PLAN_NAME},
        {"intermediate_layers", steps_index},
    };
    write_json(dir / META_NAME, meta);
    return meta;
}

json ArtifactStore::get(const std::string& id) const {
    const fs::path target = dir_of(id) / META_NAME;
    if (!fs::exists(target)) throw GisError(E_INPUT_MISSING, "Artifact 不存在:" + id, json{{"id", id}});
    return read_json(target);
}

void ArtifactStore::overwrite_meta(const std::string& id, const json& meta) const { write_json(dir_of(id) / META_NAME, meta); }

fs::path ArtifactStore::resolve_result_path(const std::string& id) const {
    const json meta = get(id);
    const std::string fmt = meta.contains("result") ? meta["result"].value("format", "gpkg") : "gpkg";
    const fs::path p = result_path_of(id, fmt);
    if (!fs::exists(p)) throw GisError(E_INPUT_MISSING, "Artifact 结果文件缺失:" + id, json{{"id", id}});
    return p;
}

std::vector<std::string> ArtifactStore::list_ids() const {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(root_, ec)) {
        const std::string name = u8str(entry.path().filename());
        if (entry.is_directory() && std::regex_match(name, ARTIFACT_ID_RE)) out.push_back(name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace gis
