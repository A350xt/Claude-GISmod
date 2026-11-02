#include "common.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <random>
#include <sstream>

namespace gis {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return c < 0x80 ? static_cast<char>(std::tolower(c)) : static_cast<char>(c); });
    return s;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])) && static_cast<unsigned char>(s[b]) < 0x80) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])) && static_cast<unsigned char>(s[e - 1]) < 0x80) --e;
    return s.substr(b, e - b);
}

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

bool ends_with_ci(std::string_view s, std::string_view suffix) {
    if (s.size() < suffix.size()) return false;
    return lower(std::string(s.substr(s.size() - suffix.size()))) == lower(std::string(suffix));
}

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::string join(const std::vector<std::string>& parts, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

std::string py_float(double v) {
    if (std::isnan(v)) return "nan";
    if (std::isinf(v)) return v > 0 ? "inf" : "-inf";
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, v);
    std::string out(buf, r.ptr);
    if (out.find_first_of(".en") == std::string::npos) out += ".0";
    return out;
}

std::string fmt_g(double v) {
    if (std::isnan(v)) return "nan";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

std::string fmt_fixed(double v, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

std::string fmt_thousands(double v) {
    if (!std::isfinite(v)) return fmt_g(v);
    long long n = std::llround(v);
    bool neg = n < 0;
    std::string digits = std::to_string(neg ? -n : n);
    std::string out;
    int count = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        if (count && count % 3 == 0) out.push_back(',');
        out.push_back(*it);
        ++count;
    }
    if (neg) out.push_back('-');
    std::reverse(out.begin(), out.end());
    return out;
}

double round_to(double v, int decimals) {
    if (!std::isfinite(v)) return v;
    const double f = std::pow(10.0, decimals);
    // 与 Python round 一样按银行家舍入在实践中差异极小；这里取最近值
    return std::nearbyint(v * f) / f;
}

json num(double v) {
    if (!std::isfinite(v)) return nullptr;
    return v;
}

std::string repr(const json& v) {
    if (v.is_null()) return "None";
    if (v.is_string()) return "'" + v.get<std::string>() + "'";
    if (v.is_boolean()) return v.get<bool>() ? "True" : "False";
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number()) {
        double d = v.get<double>();
        if (d == std::floor(d) && std::fabs(d) < 1e15) return fmt_fixed(d, 1);
        return fmt_g(d);
    }
    return v.dump();
}

std::string now_iso() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}

std::string random_token(int n) {
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    static std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> pick(0, 35);
    std::string out;
    for (int i = 0; i < n; ++i) out.push_back(alphabet[pick(rng)]);
    return out;
}

std::string base64(const std::vector<unsigned char>& bytes) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        unsigned v = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
        out.push_back(table[(v >> 18) & 63]);
        out.push_back(table[(v >> 12) & 63]);
        out.push_back(table[(v >> 6) & 63]);
        out.push_back(table[v & 63]);
    }
    if (i < bytes.size()) {
        unsigned v = bytes[i] << 16;
        if (i + 1 < bytes.size()) v |= bytes[i + 1] << 8;
        out.push_back(table[(v >> 18) & 63]);
        out.push_back(table[(v >> 12) & 63]);
        out.push_back(i + 1 < bytes.size() ? table[(v >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

}  // namespace gis
