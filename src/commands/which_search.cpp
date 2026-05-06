// `luban which <alias>` — 在 installed.json 里找 alias，打印绝对 exe 路径。
// `luban search <pattern>` — 包 vcpkg search 子命令。
// `luban run <cmd> [args...]` — uv-style 透传执行：activate luban env + exec cmd。

#include <cstdlib>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#endif

#include "../cli.hpp"
#include "../env_snapshot.hpp"
#include "../log.hpp"
#include "../path_search.hpp"
#include "../paths.hpp"
#include "../proc.hpp"
#include "../registry.hpp"

namespace luban::commands {

namespace {

namespace fs = std::filesystem;

int run_which(const cli::ParsedArgs& a) {
    if (a.positional.empty()) {
        log::err("usage: luban which <alias>");
        return 2;
    }
    const std::string& alias = a.positional[0];

    auto hit = registry::resolve_alias(alias);
    if (!hit) {
        log::errf("no alias '{}' in installed.json", alias);
        log::infof("hint: `luban doctor` lists installed components.");
        return 1;
    }

    // 主输出走 stdout（脚本可 pipe）：绝对路径
    std::cout << hit->exe.string() << '\n';

    // 辅助信息走 stderr
    log::infof("from component: {}", hit->component);
    if (hit->all_components.size() > 1) {
        std::string joined;
        for (size_t i = 0; i < hit->all_components.size(); ++i) {
            if (i) joined += ", ";
            joined += hit->all_components[i];
        }
        log::warnf("alias '{}' is provided by {} components: {} (using first)",
                   alias, hit->all_components.size(), joined);
    }

    std::error_code ec;
    if (!fs::exists(hit->exe, ec)) {
        log::warnf("path does not exist on disk! Re-apply with `luban bp apply main/cpp-base --update` "
                   "(component: {})",
                   hit->component);
        return 1;
    }
    return 0;
}

int run_run(const cli::ParsedArgs& a) {
    if (a.positional.empty()) {
        log::err("usage: luban run <cmd> [args...]");
        log::info("examples:  luban run cmake --version");
        log::info("           luban run clang -E -dM -x c nul");
        return 2;
    }
    std::string cmd_name = a.positional[0];

    // Resolve via registry first — bypasses any one-layer-of-cmd.exe overhead
    // from .cmd shims, and prevents same-named PATH entries from winning over
    // luban-managed binaries.
    fs::path exe;
    if (auto hit = registry::resolve_alias(cmd_name)) {
        exe = hit->exe;
    } else {
        // Fallback: search PATH augmented with luban's toolchain dirs.
        // path_search::on_path reads the current process PATH, so we temporarily
        // splice luban's toolchain dirs in front, search, and restore.
#ifdef _WIN32
        std::string current_path;
        if (const char* p = std::getenv("PATH"); p) current_path = p;
        std::string augmented;
        for (auto& d : env_snapshot::path_dirs()) {
            if (!augmented.empty()) augmented.push_back(';');
            augmented += d.string();
        }
        if (!current_path.empty()) augmented += ';' + current_path;
        SetEnvironmentVariableA("PATH", augmented.c_str());

        if (auto hit = path_search::on_path(cmd_name)) exe = *hit;

        // restore PATH
        SetEnvironmentVariableA("PATH", current_path.c_str());
#else
        if (auto hit = path_search::on_path(cmd_name)) exe = *hit;
#endif
    }

    if (exe.empty() || !fs::exists(exe)) {
        log::errf("'{}' not found in registry or PATH", cmd_name);
        log::info("hint: `luban which <alias>` to debug, or `luban bp apply main/cpp-base` to install the toolchain");
        return 127;
    }

    // 组合 cmd + 透传 args
    std::vector<std::string> argv;
    argv.push_back(exe.string());
    for (size_t i = 1; i < a.positional.size(); ++i) argv.push_back(a.positional[i]);

    // 注入 luban env：toolchain dirs 加 PATH 头部 + LUBAN_*
    auto env_overrides = env_snapshot::apply_to({});
    return proc::run(argv, fs::current_path().string(), env_overrides);
}

int run_search(const cli::ParsedArgs& a) {
    if (a.positional.empty()) {
        log::err("usage: luban search <pattern>");
        return 2;
    }
    const std::string& pattern = a.positional[0];

    auto vcpkg = registry::resolve_alias("vcpkg");
    if (!vcpkg) {
        log::err("vcpkg is not installed (no alias 'vcpkg' in installed.json).");
        log::info("install it via:  luban bp apply main/cpp-base   # bundles vcpkg + bootstrap");
        return 1;
    }

    std::vector<std::string> cmd = {vcpkg->exe.string(), "search", pattern};
    auto env_overrides = env_snapshot::apply_to({});
    return proc::run(cmd, fs::current_path().string(), env_overrides);
}

}  // namespace

void register_run() {
    cli::Subcommand c;
    c.name = "run";
    c.help = "run a luban-managed exe with toolchain env injected (uv-style)";
    c.group = "advanced";
    c.long_help =
        "  `luban run <cmd> [args...]` injects luban's toolchain env (PATH +\n"
        "  VCPKG_ROOT + vcpkg cache vars) into a child process and exec's the\n"
        "  command, forwarding all args verbatim. Useful when you haven't run\n"
        "  `luban env --user` (e.g., in a fresh CI shell or container).\n"
        "\n"
        "  `<cmd>` resolves via:\n"
        "    1. registry alias (e.g., 'cmake' → toolchains/cmake-X/bin/cmake.exe)\n"
        "    2. fallback PATH search using luban-augmented PATH\n"
        "\n"
        "  All args after `<cmd>` are forwarded WITHOUT luban-side parsing\n"
        "  (so `luban run cmake --version` works, the --version goes to cmake).";
    c.examples = {
        "luban run cmake --version\tWithout `luban env --user` first",
        "luban run clang -E -dM -x c nul\tDump preprocessor macros",
        "luban run vcpkg list\tList installed ports",
    };
    c.forward_rest = true;
    c.run = run_run;
    cli::register_subcommand(std::move(c));
}

void register_which() {
    cli::Subcommand c;
    c.name = "which";
    c.help = "show which exe an alias resolves to (from installed.json)";
    c.group = "advanced";
    c.long_help =
        "  Resolve an alias (cmake / clangd / vcpkg / ...) via installed.json\n"
        "  and print its absolute path on stdout.\n"
        "\n"
        "  Useful for debugging PATH issues, confirming versions, or scripting.\n"
        "  Note: this only finds aliases LUBAN installed; system PATH is not consulted.";
    c.n_positional = 1;
    c.positional_names = {"alias"};
    c.examples = {
        "luban which cmake\tPrints the cmake.exe absolute path",
        "luban which clangd\tFor clangd debugging",
        "luban which vcpkg\tFind the vcpkg.exe luban bootstrapped",
    };
    c.run = run_which;
    cli::register_subcommand(std::move(c));
}

void register_search() {
    cli::Subcommand c;
    c.name = "search";
    c.help = "search vcpkg ports (wraps `vcpkg search`)";
    c.group = "dep";
    c.long_help =
        "  Run `vcpkg search <pattern>` and print the results.\n"
        "  Requires vcpkg to be installed (`luban bp apply main/cpp-base`).\n"
        "\n"
        "  vcpkg's search matches port name + description substring; it's not\n"
        "  fuzzy. The first column of output is the port name you'd pass to\n"
        "  `luban add`.";
    c.n_positional = 1;
    c.positional_names = {"pattern"};
    c.examples = {
        "luban search fmt\tList everything matching 'fmt' (fmt, fmtlog, libfmt-cpp ...)",
        "luban search json\tCommon: nlohmann-json, rapidjson, jsoncpp, simdjson, ...",
        "luban search boost-asio\tPort name match for Boost.Asio",
    };
    c.run = run_search;
    cli::register_subcommand(std::move(c));
}

}  // namespace luban::commands
