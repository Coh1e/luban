// `luban run <cmd> [args...]` — uv-style 透传执行：activate luban env + exec cmd.

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

namespace luban::commands {

namespace {

namespace fs = std::filesystem;

int run_run(const cli::ParsedArgs& a) {
    if (a.positional.empty()) {
        log::err("usage: luban run <cmd> [args...]");
        log::info("examples:  luban run cmake --version");
        log::info("           luban run clang -E -dM -x c nul");
        return 2;
    }
    std::string cmd_name = a.positional[0];

    // PATH search augmented with luban's toolchain dirs. Splice luban's
    // toolchain dirs in front of the current PATH, search, then restore.
    fs::path exe;
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

    SetEnvironmentVariableA("PATH", current_path.c_str());
#else
    if (auto hit = path_search::on_path(cmd_name)) exe = *hit;
#endif

    if (exe.empty() || !fs::exists(exe)) {
        log::errf("'{}' not found in PATH (with luban toolchain dirs prepended)", cmd_name);
        log::info("hint: `luban bp apply main/cpp-toolchain` to install the toolchain");
        return 127;
    }

    std::vector<std::string> argv;
    argv.push_back(exe.string());
    for (size_t i = 1; i < a.positional.size(); ++i) argv.push_back(a.positional[i]);

    auto env_overrides = env_snapshot::apply_to({});
    return proc::run(argv, fs::current_path().string(), env_overrides);
}

}  // namespace

void register_run() {
    cli::Subcommand c;
    c.name = "run";
    c.help = "run a luban-managed exe with toolchain env injected (uv-style)";
    c.group = "utility";
    c.long_help =
        "  `luban run <cmd> [args...]` injects luban's toolchain env (PATH +\n"
        "  VCPKG_ROOT + vcpkg cache vars) into a child process and exec's the\n"
        "  command, forwarding all args verbatim. Useful when you haven't run\n"
        "  `luban env --user` (e.g., in a fresh CI shell or container).\n"
        "\n"
        "  `<cmd>` resolves via PATH search using luban-augmented PATH.\n"
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

}  // namespace luban::commands
