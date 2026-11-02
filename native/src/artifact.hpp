// 分析产物：每次 analyze 一个目录 <workdir>/artifacts/ar_<时间戳><随机>/
//   meta.json  标题、输入、步骤摘要、告警、坐标系决策、中间图层索引
//   plan.json  原语操作序列（可回放）
//   result.gpkg / result.geojson / result.tif
//   step_<id>.tif（栅格中间步骤；矢量中间步骤是 result.gpkg 里的图层）
//
// 安全不变量：artifact id 只能是 ar_ + 10–32 位字母数字；所有路径都在 realpath 后的
// artifacts 根目录之下拼出，并在读写前再次校验包含关系。
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common.hpp"
#include "layer.hpp"
#include "raster.hpp"

namespace gis {

bool is_artifact_token(const std::string& s);
/// "ar_xxx#step" → (artifact_id, step_id)
std::optional<std::pair<std::string, std::string>> parse_step_ref(const std::string& s);

/// 一步的结果：矢量或栅格（二者互斥）
struct Data {
    std::shared_ptr<Layer> vec;
    std::shared_ptr<Raster> ras;
    bool is_raster() const { return static_cast<bool>(ras); }
};

class ArtifactStore {
public:
    explicit ArtifactStore(const fs::path& workdir);

    const fs::path& root() const { return root_; }
    fs::path dir_of(const std::string& id) const;
    fs::path result_path_of(const std::string& id, const std::string& fmt) const;
    fs::path image_path_for(const std::string& id, const std::string& stem) const;

    /// 新建空产物目录与初始 meta，返回 id
    std::string create(const std::string& kind, const std::string& title);
    json finalize_analysis(const std::string& id, const std::string& title, const json& operations, const json& steps,
                           const std::vector<std::string>& summaries, const std::vector<std::string>& warnings,
                           const json& crs_info, const json& inputs, const Data& result, const std::string& result_format,
                           const std::vector<std::pair<std::string, Data>>& intermediates);
    json get(const std::string& id) const;
    void overwrite_meta(const std::string& id, const json& meta) const;
    fs::path resolve_result_path(const std::string& id) const;
    std::vector<std::string> list_ids() const;

private:
    fs::path root_;
    fs::path checked(const fs::path& p, const char* what) const;
    void write_json(const fs::path& p, const json& payload) const;
    json read_json(const fs::path& p) const;
};

}  // namespace gis
