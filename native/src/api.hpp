// 协议动作：stdin 一条 {"action": ..., ...} → 这里的一个函数 → result。
#pragma once

#include "common.hpp"

namespace gis {

json action_inspect(const fs::path& workdir, const json& payload);
json action_analyze(const fs::path& workdir, const json& payload);
json action_show(const fs::path& workdir, const json& payload);
json action_operations(const fs::path& workdir, const json& payload);
json action_list(const fs::path& workdir, const json& payload);
json action_preview(const fs::path& workdir, const json& payload);
/// 对话卡片：随 inspect / analyze 的结果生成（失败不影响主结果）
json action_card(const fs::path& workdir, const std::string& action, const json& request, const json& result,
                 const json& opts);

}  // namespace gis
