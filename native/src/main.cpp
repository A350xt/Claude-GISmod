// claude-gis：stdin 一条 JSON 请求 → stdout 一条 JSON 封套。
//
//   echo '{"action":"inspect","path":"/data/x.geojson"}' | claude-gis run --workdir .
//
//   {"ok": true,  "result": {...}, "card": {...}?}
//   {"ok": false, "error": {"code": "...", "message": "...", "details": {...}}}
//   退出码：0 成功 / 2 领域错误 / 1 内部错误
//
// 与原 Python 内核（python -m claude_gis run）协议一致；不再提供 visualize / compose（出图已移除）。
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <cpl_conv.h>
#include <cpl_error.h>
#include <gdal_priv.h>
#include <ogr_srs_api.h>

#include "api.hpp"
#include "common.hpp"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

#ifndef CLAUDE_GIS_VERSION
#define CLAUDE_GIS_VERSION "dev"
#endif

using namespace gis;

namespace {

fs::path executable_dir() {
#ifdef _WIN32
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    buf.resize(n);
    return fs::path(buf).parent_path();
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) == 0) return fs::weakly_canonical(fs::path(buf)).parent_path();
    return fs::current_path();
#else
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) return fs::path(std::string(buf, static_cast<size_t>(n))).parent_path();
    return fs::current_path();
#endif
}

/// 只用随程序发布的 PROJ 数据库，忽略系统里其他安装（如 PostGIS 写入的 PROJ_LIB）
void setup_gdal() {
    GDALAllRegister();
    CPLSetErrorHandler(CPLQuietErrorHandler);
    CPLSetConfigOption("GDAL_FILENAME_IS_UTF8", "YES");
    CPLSetConfigOption("OGR_GEOJSON_MAX_OBJ_SIZE", "0");
    CPLSetConfigOption("SHAPE_RESTORE_SHX", "YES");
    const fs::path dir = executable_dir();
    std::vector<std::string> paths;
    for (const fs::path& cand : {dir / "share" / "proj", dir / "proj", dir}) {
        std::error_code ec;
        if (fs::exists(cand / "proj.db", ec)) paths.push_back(u8str(cand));
    }
    if (!paths.empty()) {
        std::vector<const char*> raw;
        for (const auto& p : paths) raw.push_back(p.c_str());
        raw.push_back(nullptr);
        OSRSetPROJSearchPaths(raw.data());
    }
}

std::string read_stdin() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    std::ostringstream ss;
    ss << std::cin.rdbuf();
    return ss.str();
}

void emit(const json& envelope) {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    const std::string text = envelope.dump(-1, ' ', false, json::error_handler_t::replace);
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

json error_envelope(const std::string& code, const std::string& message, const json& details = json::object()) {
    return {{"ok", false}, {"error", {{"code", code}, {"message", message}, {"details", details}}}};
}

int usage() {
    std::fprintf(stderr, "usage: claude-gis run [--workdir DIR]    (request JSON on stdin)\n       claude-gis --version\n");
    return 2;
}

int run(const std::vector<std::string>& args) {
    if (args.size() >= 2 && (args[1] == "--version" || args[1] == "version")) {
        std::printf("claude-gis %s (GDAL %s)\n", CLAUDE_GIS_VERSION, GDALVersionInfo("RELEASE_NAME"));
        return 0;
    }
    if (args.size() < 2 || args[1] != "run") return usage();
    std::string workdir_arg;
    for (size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--workdir" && i + 1 < args.size()) workdir_arg = args[++i];
        else if (starts_with(args[i], "--workdir=")) workdir_arg = args[i].substr(10);
        else return usage();
    }
    if (workdir_arg.empty())
        if (const char* env = std::getenv("CLAUDE_GIS_WORKDIR")) workdir_arg = env;

    setup_gdal();

    json payload;
    try {
        const std::string raw = read_stdin();
        payload = trim(raw).empty() ? json::object() : json::parse(raw);
    } catch (const std::exception& exc) {
        emit(error_envelope(E_BAD_REQUEST, std::string("请求不是合法 JSON：") + exc.what()));
        return 2;
    }
    if (!payload.is_object()) {
        emit(error_envelope(E_BAD_REQUEST, "请求必须是 JSON 对象"));
        return 2;
    }

    const std::string action = payload.value("action", "");
    payload.erase("action");
    const json card_opts = payload.contains("_card") ? payload["_card"] : json(nullptr);
    payload.erase("_card");
    if (payload.contains("workdir") && payload["workdir"].is_string()) {
        workdir_arg = payload["workdir"].get<std::string>();
        payload.erase("workdir");
    }
    const fs::path workdir = u8path(workdir_arg.empty() ? "." : workdir_arg);

    using Handler = std::function<json(const fs::path&, const json&)>;
    static const std::map<std::string, Handler> handlers = {
        {"inspect", action_inspect}, {"analyze", action_analyze}, {"show", action_show},
        {"operations", action_operations}, {"list", action_list}, {"preview", action_preview},
    };
    const auto it = handlers.find(action);
    if (it == handlers.end()) {
        json available = json::array();
        for (const auto& [name, _] : handlers) available.push_back(name);
        const std::string hint = (action == "visualize" || action == "compose") ? "（原生内核不再提供出图）" : "";
        emit(error_envelope(E_BAD_REQUEST, "未知 action：" + action + hint, json{{"available", available}}));
        return 2;
    }

    try {
        const json request = payload;
        json result = it->second(workdir, payload);
        json envelope = {{"ok", true}, {"result", result}};
        if (card_opts.is_object()) {
            // 卡片只给界面用：失败不影响主结果
            try {
                envelope["card"] = action_card(workdir, action, request, result, card_opts);
            } catch (const std::exception& exc) {
                envelope["card"] = {{"error", exc.what()}};
            }
        }
        emit(envelope);
        return 0;
    } catch (const GisError& exc) {
        emit(error_envelope(exc.code(), exc.what(), exc.details()));
        return exc.code() == E_INTERNAL ? 1 : 2;
    } catch (const std::exception& exc) {
        emit(error_envelope(E_INTERNAL, std::string("内部错误：") + exc.what()));
        return 1;
    }
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
        if (n > 1) WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, s.data(), n, nullptr, nullptr);
        args.push_back(std::move(s));
    }
    return run(args);
}
#else
int main(int argc, char** argv) { return run(std::vector<std::string>(argv, argv + argc)); }
#endif
