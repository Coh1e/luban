// `luban add <pkg>[@version]` / `luban remove <pkg>`
//
// 不变量 8：toolchain 名（cmake/ninja/clang 等）拒绝加进 vcpkg.json，
// 引导到 `bp apply main/cpp-toolchain`。
//
// 共用：
//   1) 找项目根（cwd 起向上找 vcpkg.json）
//   2) 读 vcpkg.json → 改 → 写
//   3) 重新生成 luban.cmake

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>

#include "../cli.hpp"
#include "../log.hpp"
#include "../luban_cmake_gen.hpp"
#include "../vcpkg_manifest.hpp"

namespace luban::commands {

namespace {

namespace fs = std::filesystem;

// 拒绝列表：这些是系统层 toolchain，不该进 vcpkg.json。
constexpr std::array<const char*, 14> kSystemTools = {
    "cmake", "ninja", "clang", "clang++", "clangd", "clang-format", "clang-tidy",
    "lld", "llvm-mingw", "mingit", "git", "vcpkg", "make", "gcc",
};

bool is_system_tool(const std::string& name) {
    for (auto* t : kSystemTools) if (name == t) return true;
    return false;
}

// 找项目根：cwd 向上一直到根，第一个含 vcpkg.json 的目录。
// 找不到 = cwd 本身（让用户在 cwd 起一个新项目）。
fs::path find_project_root() {
    std::error_code ec;
    fs::path d = fs::current_path(ec);
    while (!d.empty()) {
        if (fs::exists(d / "vcpkg.json", ec)) return d;
        fs::path parent = d.parent_path();
        if (parent == d) break;
        d = parent;
    }
    return fs::current_path(ec);
}

void regenerate(const fs::path& project_dir) {
    auto targets = luban_cmake_gen::read_targets_from_cmake(project_dir);
    if (targets.empty()) {
        // 兜底：从项目名推一个
        targets.push_back(project_dir.filename().string());
    }
    luban_cmake_gen::regenerate_in_project(project_dir, targets);
}

int run_add(const cli::ParsedArgs& a) {
    if (a.positional.empty()) {
        log::err("usage: luban add <pkg>[@version]");
        return 2;
    }
    auto [pkg, ver] = vcpkg_manifest::parse_pkg_spec(a.positional[0]);
    if (pkg.empty()) {
        log::err("empty package name");
        return 2;
    }
    if (is_system_tool(pkg)) {
        log::errf("'{}' is a system-level toolchain, not a vcpkg library.", pkg);
        log::infof("System tools live in <data>/toolchains/. Use:");
        log::infof("  luban bp apply main/cpp-base   # ships {} alongside the rest", pkg);
        return 2;
    }

    fs::path proj = find_project_root();
    fs::path vcpkg_path = proj / "vcpkg.json";

    auto m = vcpkg_manifest::load(vcpkg_path, proj.filename().string());
    vcpkg_manifest::add(m, pkg, ver);
    vcpkg_manifest::save(vcpkg_path, m);
    log::okf("added {} to vcpkg.json{}", pkg, ver.empty() ? "" : " (>= " + ver + ")");

    regenerate(proj);
    log::okf("regenerated luban.cmake");

    log::info("next: luban build  (cmake will fetch the dep via vcpkg manifest mode)");
    log::info("note: ensure VCPKG_ROOT is set + CMakePresets has CMAKE_TOOLCHAIN_FILE");
    return 0;
}

int run_remove(const cli::ParsedArgs& a) {
    if (a.positional.empty()) {
        log::err("usage: luban remove <pkg>");
        return 2;
    }
    const std::string& pkg = a.positional[0];

    fs::path proj = find_project_root();
    fs::path vcpkg_path = proj / "vcpkg.json";
    auto m = vcpkg_manifest::load(vcpkg_path, proj.filename().string());
    if (!vcpkg_manifest::remove(m, pkg)) {
        log::warnf("'{}' was not in vcpkg.json (no change)", pkg);
        return 0;
    }
    vcpkg_manifest::save(vcpkg_path, m);
    log::okf("removed {} from vcpkg.json", pkg);

    regenerate(proj);
    log::okf("regenerated luban.cmake");
    return 0;
}

}  // namespace

void register_add() {
    cli::Subcommand c;
    c.name = "add";
    c.help = "add a vcpkg library (edits vcpkg.json + luban.cmake)";
    c.group = "dep";
    c.long_help =
        "  Add a vcpkg port to the current project's vcpkg.json and regenerate\n"
        "  luban.cmake (find_package + target_link_libraries auto-wired).\n"
        "\n"
        "  Version constraint: `luban add fmt@10` → vcpkg `version>=: 10.0.0`.\n"
        "\n"
        "  System tools (cmake, ninja, clang, ...) are rejected — those are\n"
        "  managed via `luban bp apply main/cpp-base`, not vcpkg.";
    c.n_positional = 1;
    c.positional_names = {"pkg[@version]"};
    c.examples = {
        "luban add fmt\tAdd fmt at baseline version",
        "luban add spdlog@1.13\tAdd spdlog with version>=1.13.0",
        "luban add boost-asio\tAdd Boost.Asio (Boost::asio target)",
    };
    c.run = run_add;
    cli::register_subcommand(std::move(c));
}

void register_remove() {
    cli::Subcommand c;
    c.name = "remove";
    c.help = "remove a vcpkg library (edits vcpkg.json + luban.cmake)";
    c.group = "dep";
    c.n_positional = 1;
    c.positional_names = {"pkg"};
    c.examples = {
        "luban remove fmt\tRemove fmt from vcpkg.json + luban.cmake",
    };
    c.run = run_remove;
    cli::register_subcommand(std::move(c));
}

}  // namespace luban::commands
