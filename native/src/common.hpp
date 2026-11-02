// 公共基础：错误码与领域异常、JSON、UTF-8 路径、数字格式化。
#pragma once

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace gis {

using json = nlohmann::ordered_json;
namespace fs = std::filesystem;

// ---------------------------------------------------------------- 错误码（稳定契约，见 docs/tool-api.md）
inline constexpr const char* E_BAD_REQUEST = "E_BAD_REQUEST";
inline constexpr const char* E_INPUT_MISSING = "E_INPUT_MISSING";
inline constexpr const char* E_CRS_AMBIGUOUS = "E_CRS_AMBIGUOUS";
inline constexpr const char* E_OP_UNKNOWN = "E_OP_UNKNOWN";
inline constexpr const char* E_GEOMETRY_INVALID_UNFIXABLE = "E_GEOMETRY_INVALID_UNFIXABLE";
inline constexpr const char* E_EMPTY_RESULT = "E_EMPTY_RESULT";
inline constexpr const char* E_OUTPUT_ERROR = "E_OUTPUT_ERROR";
inline constexpr const char* E_INTERNAL = "E_INTERNAL";

/// 领域错误：带稳定错误码与可修复细节，统一走封套输出。
class GisError : public std::runtime_error {
public:
    GisError(std::string code, const std::string& message, json details = json::object())
        : std::runtime_error(message), code_(std::move(code)), details_(std::move(details)) {}
    const std::string& code() const { return code_; }
    json& details() { return details_; }
    const json& details() const { return details_; }

private:
    std::string code_;
    json details_;
};

inline constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

// ---------------------------------------------------------------- UTF-8 路径
/// 协议里的路径都是 UTF-8；Windows 上必须经 char8_t 构造，不能按本地代码页解释。
inline fs::path u8path(const std::string& s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}
inline std::string u8str(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}
/// 与 Python 的 str(Path) 一致：Windows 上用反斜杠。
inline std::string native_str(const fs::path& p) { return u8str(p.lexically_normal().make_preferred()); }

// ---------------------------------------------------------------- 字符串
std::string lower(std::string s);
std::string trim(const std::string& s);
bool starts_with(std::string_view s, std::string_view prefix);
bool ends_with_ci(std::string_view s, std::string_view suffix);
std::string replace_all(std::string s, const std::string& from, const std::string& to);
std::string join(const std::vector<std::string>& parts, const std::string& sep);

/// Python repr(float)：最短可往返表示，整数值带 ".0"
std::string py_float(double v);
/// Python 的 f"{x:g}"
std::string fmt_g(double v);
/// 固定小数位：f"{x:.3f}"
std::string fmt_fixed(double v, int decimals);
/// 千分位整数：f"{x:,.0f}"
std::string fmt_thousands(double v);
/// Python 的 round(x, n)，NaN 原样
double round_to(double v, int decimals);
/// 数字转 JSON（NaN/Inf → null，与 Python json_safe 一致）
json num(double v);
/// Python repr 风格的短文本（用于 summary 中的 true=…/false=…）
std::string repr(const json& v);

/// 当前本地时间 ISO（秒精度），与 Python datetime.now().isoformat(timespec="seconds") 一致
std::string now_iso();
/// 随机小写字母+数字
std::string random_token(int n);

std::string base64(const std::vector<unsigned char>& bytes);

}  // namespace gis
