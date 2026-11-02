// 原语词表：op 名 → 处理函数。矢量算子在 ops_vector.cpp，栅格算子在 ops_raster.cpp。
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "common.hpp"
#include "layer.hpp"
#include "raster.hpp"
#include "resolver.hpp"

namespace gis {

/// 一步的结果：矢量 vec 与栅格 ras 二选一；table 为结构化明细（进 steps[].table）
struct StepResult {
    std::shared_ptr<Layer> vec;
    std::shared_ptr<Raster> ras;
    std::vector<std::string> summaries;
    json table;  // null = 无
};

using OpHandler = std::function<StepResult(Resolver&, const json& params, const std::string& step_id)>;

const std::map<std::string, OpHandler>& op_handlers();
void register_vector_ops(std::map<std::string, OpHandler>& ops);
void register_raster_ops(std::map<std::string, OpHandler>& ops);

// ---------------------------------------------------------------- 参数工具
/// 必填参数；缺失时抛 E_BAD_REQUEST「操作 X 缺少必填参数 Y」
const json& need(const json& params, const char* key, const char* op);
bool has(const json& params, const char* key);  // 存在且非 null
double to_double(const json& v, const char* key);
int to_int(const json& v, const char* key);
bool to_bool(const json& v);
std::string to_str(const json& v);
double opt_double(const json& params, const char* key, double fallback);
int opt_int(const json& params, const char* key, int fallback);
std::string opt_str(const json& params, const char* key, const std::string& fallback);
std::vector<std::string> opt_str_list(const json& params, const char* key, const std::vector<std::string>& fallback);

/// Python 列表 repr：['a', 'b'] / [1.0, 2.0]
std::string py_list(const std::vector<std::string>& items);
std::string py_list(const std::vector<double>& items);

}  // namespace gis
