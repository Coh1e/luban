// `luban build` — wraps cmake configure + build, syncs compile_commands.json.
// Mirrors luban_boot/commands/build.py.

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <system_error>
#include <vector>

#include <fstream>
#include <sstream>

#include "../cli.hpp"
#include "../env_snapshot.hpp"
#include "../file_util.hpp"
#include "json.hpp"
#include "../log.hpp"
#include "../luban_toml.hpp"
#include "../paths.hpp"
#include "../proc.hpp"
#include "../registry.hpp"
#include "../vcpkg_manifest.hpp"

namespace luban::commands {

namespace fs = std::filesystem;

// 公共入口：让 commands/new_project.cpp 末尾能直接调，不必走 cli dispatch。
// 返回 cmake 退出码（0 = 成功）。
int build_project(const fs::path& project_dir);

namespace {

// Find the real cmake.exe via the registry, bypassing .cmd shim. Falls back
// to "cmake" on PATH.
std::string cmake_exe() {
    auto recs = registry::load_installed();
    auto it = recs.find("cmake");
    if (it != recs.end()) {
        fs::path root = paths::toolchain_dir(it->second.toolchain_dir);
        for (auto& [alias, rel] : it->second.bins) {
            if (alias == "cmake") {
                std::string norm = rel;
                for (auto& c : norm) if (c == '/' || c == '\\') c = static_cast<char>(fs::path::preferred_separator);
                return (root / norm).string();
            }
        }
    }
    fs::path shim = paths::bin_dir() / "cmake.cmd";
    std::error_code ec;
    if (fs::exists(shim, ec)) return shim.string();
    return "cmake";
}

std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) { if (i) s.push_back(' '); s += v[i]; }
    return s;
}

int run_cmd(const std::vector<std::string>& cmd, const fs::path& cwd,
            const std::map<std::string, std::string>& env_overrides) {
    log::step(join(cmd));
    return proc::run(cmd, cwd.string(), env_overrides);
}

fs::path copy_compile_commands(const fs::path& project, const std::string& preset) {
    fs::path src = project / "build" / preset / "compile_commands.json";
    std::error_code ec;
    if (!fs::exists(src, ec)) return {};
    fs::path dst = project / "compile_commands.json";
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) return {};
    return dst;
}

// 项目 CMakePresets.json 是否声明了 wasm preset。`luban new --target=wasm`
// 生成的项目里有；普通项目没有。
bool has_wasm_preset(const fs::path& project) {
    fs::path p = project / "CMakePresets.json";
    std::error_code ec;
    if (!fs::exists(p, ec)) return false;
    std::string text = file_util::read_text_no_bom(p);
    nlohmann::json doc;
    try { doc = nlohmann::json::parse(text); } catch (...) { return false; }
    if (!doc.is_object() || !doc.contains("configurePresets")) return false;
    for (auto& cp : doc["configurePresets"]) {
        if (cp.is_object() && cp.contains("name") && cp["name"].is_string()
            && cp["name"].get<std::string>() == "wasm") return true;
    }
    return false;
}

// 自动选 preset：
//   - 用户显式 --preset xxx → 直接用
//   - 项目有 wasm preset → "wasm" (emscripten 路径)
//   - 项目 vcpkg.json 有 deps → "default"（vcpkg toolchain）
//   - 否则 → "no-vcpkg"（hello-world fast path，无需 VCPKG_ROOT）
std::string pick_preset(const fs::path& project, const std::string& explicit_preset) {
    if (!explicit_preset.empty() && explicit_preset != "auto") return explicit_preset;
    if (has_wasm_preset(project)) return "wasm";
    fs::path vcpkg_json = project / "vcpkg.json";
    std::error_code ec;
    if (!fs::exists(vcpkg_json, ec)) return "default";
    auto m = vcpkg_manifest::load(vcpkg_json);
    return m.dependencies.empty() ? "no-vcpkg" : "default";
}

// emcmake.bat path from the installed emscripten component. Returns empty
// if emscripten not installed (caller surfaces a friendlier error).
std::string emcmake_bat() {
    auto recs = registry::load_installed();
    auto it = recs.find("emscripten");
    if (it == recs.end()) return "";
    fs::path root = paths::toolchain_dir(it->second.toolchain_dir);
    for (auto& [alias, rel] : it->second.bins) {
        if (alias == "emcmake") {
            std::string norm = rel;
            for (auto& c : norm) if (c == '/' || c == '\\') c = static_cast<char>(fs::path::preferred_separator);
            return (root / norm).string();
        }
    }
    return "";
}

bool is_wasm_preset(const std::string& p) {
    return p == "wasm" || p == "wasm-debug";
}

// OQ-2: per-project toolchain version pin check. Reads `[toolchain]` from
// the project's luban.toml and compares each entry to the installed registry.
// Empty pins map → no-op. Mismatches log a warning and continue (we don't
// hard-fail; the user may have intentionally upgraded). Unknown components
// (in pins but not in installed.json) also warn — they signal either a typo
// or a missing `luban setup`.
//
// This runs *before* cmake so the warnings surface above the configure noise
// rather than getting buried under it.
void check_toolchain_pins(const fs::path& project) {
    fs::path toml_path = project / "luban.toml";
    std::error_code ec;
    if (!fs::exists(toml_path, ec)) return;  // no luban.toml → no pins

    auto cfg = luban::luban_toml::load(toml_path);
    if (cfg.toolchain.empty()) return;       // no [toolchain] section → no-op

    auto installed = registry::load_installed();
    for (auto const& [name, want] : cfg.toolchain) {
        auto it = installed.find(name);
        if (it == installed.end()) {
            log::warnf("luban.toml [toolchain] pins {} = \"{}\", but {} is not installed. "
                       "Run: luban bp apply main/cpp-base   # installs {} alongside others",
                       name, want, name, name);
            continue;
        }
        std::string have = it->second.version;
        if (have != want) {
            log::warnf("luban.toml [toolchain] pins {} = \"{}\", but installed is \"{}\". "
                       "Build proceeds; reinstall the component to match.",
                       name, want, have);
        }
    }
}

int run_build(const cli::ParsedArgs& args) {
    fs::path project_arg = args.opts.count("at") ? fs::path(args.opts.at("at")) : fs::current_path();
    std::error_code ec;
    fs::path project = fs::absolute(project_arg, ec);
    if (!fs::exists(project / "CMakeLists.txt", ec)) {
        log::errf("no CMakeLists.txt in {}", project.string());
        return 2;
    }

    check_toolchain_pins(project);

    std::string cmake = cmake_exe();
    // 用户没传 --preset 时（默认值 "auto"）智能挑选；显式传了用显式值。
    std::string preset_raw = args.opts.count("preset") ? args.opts.at("preset") : std::string("auto");
    std::string preset = pick_preset(project, preset_raw);
    if (preset_raw == "auto") log::infof("preset: {} (auto-picked from vcpkg.json deps)", preset);

    auto env_overrides = env_snapshot::apply_to({});

    // Configure. For wasm presets we wrap with emcmake.bat so it injects the
    // emscripten cmake toolchain file before invoking cmake. After the first
    // configure, the toolchain file is cached in CMakeCache, so subsequent
    // builds don't need emcmake.
    std::vector<std::string> configure_cmd;
    if (is_wasm_preset(preset)) {
        std::string emcmake = emcmake_bat();
        if (emcmake.empty()) {
            log::errf("preset '{}' needs emscripten, but it is not installed.", preset);
            log::err("run: luban setup --with emscripten");
            return 2;
        }
        configure_cmd = {emcmake, cmake, "--preset", preset};
    } else {
        configure_cmd = {cmake, "--preset", preset};
    }
    int rc = run_cmd(configure_cmd, project, env_overrides);
    if (rc != 0) {
        log::errf("cmake --preset {} returned {}", preset, rc);
        return rc;
    }

    // Build
    rc = run_cmd({cmake, "--build", "--preset", preset}, project, env_overrides);
    if (rc != 0) {
        log::errf("cmake --build --preset {} returned {}", preset, rc);
        return rc;
    }

    fs::path cc = copy_compile_commands(project, preset);
    if (!cc.empty()) log::okf("compile_commands.json synced \xe2\x86\x92 {}", cc.string());
    else log::warn("compile_commands.json not found in build dir \xe2\x80\x94 clangd will be unhappy");

    log::ok("build complete");
    return 0;
}

}  // namespace

int build_project(const fs::path& project_dir) {
    cli::ParsedArgs a;
    a.opts["preset"] = "auto";
    a.opts["at"] = project_dir.string();
    return run_build(a);
}

void register_build() {
    cli::Subcommand c;
    c.name = "build";
    c.help = "configure + build via cmake/ninja, sync compile_commands.json";
    c.group = "project";
    c.long_help =
        "  Wraps `cmake --preset <P> && cmake --build --preset <P>`.\n"
        "  After build, copies build/<preset>/compile_commands.json to project root\n"
        "  so clangd / VS Code C/C++ extension auto-find it.\n"
        "\n"
        "  Preset auto-selection (default --preset=auto):\n"
        "    - 'wasm' preset present    → preset 'wasm'      (wraps cmake with emcmake)\n"
        "    - vcpkg.json has deps      → preset 'default'   (uses VCPKG_ROOT)\n"
        "    - vcpkg.json deps empty    → preset 'no-vcpkg'  (no toolchain file)\n"
        "  Override with --preset=release, --preset=default, etc.";
    c.opts = {{"preset", "auto"}, {"at", "."}};
    c.examples = {
        "luban build\tAuto-pick preset based on vcpkg.json",
        "luban build --preset release\tForce release preset",
        "luban build --at ../other\tBuild a different project dir",
    };
    c.run = run_build;
    cli::register_subcommand(std::move(c));
}

}  // namespace luban::commands
