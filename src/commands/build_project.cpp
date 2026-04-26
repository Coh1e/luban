// `luban build` — wraps cmake configure + build, syncs compile_commands.json.
// Mirrors luban_boot/commands/build.py.

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <system_error>
#include <vector>

#include "../cli.hpp"
#include "../env_snapshot.hpp"
#include "../log.hpp"
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

// 自动选 preset：
//   - 用户显式 --preset xxx → 直接用
//   - 项目 vcpkg.json 有 deps → "default"（vcpkg toolchain）
//   - 否则 → "no-vcpkg"（hello-world fast path，无需 VCPKG_ROOT）
std::string pick_preset(const fs::path& project, const std::string& explicit_preset) {
    if (!explicit_preset.empty() && explicit_preset != "auto") return explicit_preset;
    fs::path vcpkg_json = project / "vcpkg.json";
    std::error_code ec;
    if (!fs::exists(vcpkg_json, ec)) return "default";
    auto m = vcpkg_manifest::load(vcpkg_json);
    return m.dependencies.empty() ? "no-vcpkg" : "default";
}

int run_build(const cli::ParsedArgs& args) {
    fs::path project_arg = args.opts.count("at") ? fs::path(args.opts.at("at")) : fs::current_path();
    std::error_code ec;
    fs::path project = fs::absolute(project_arg, ec);
    if (!fs::exists(project / "CMakeLists.txt", ec)) {
        log::errf("no CMakeLists.txt in {}", project.string());
        return 2;
    }

    std::string cmake = cmake_exe();
    // 用户没传 --preset 时（默认值 "auto"）智能挑选；显式传了用显式值。
    std::string preset_raw = args.opts.count("preset") ? args.opts.at("preset") : std::string("auto");
    std::string preset = pick_preset(project, preset_raw);
    if (preset_raw == "auto") log::infof("preset: {} (auto-picked from vcpkg.json deps)", preset);

    auto env_overrides = env_snapshot::apply_to({});

    // Configure
    int rc = run_cmd({cmake, "--preset", preset}, project, env_overrides);
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
