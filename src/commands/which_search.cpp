// `luban which <alias>` — 在 installed.json 里找 alias，打印绝对 exe 路径。
// `luban search <pattern>` — 包 vcpkg search 子命令。

#include <iostream>

#include "../cli.hpp"
#include "../env_snapshot.hpp"
#include "../log.hpp"
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
        log::warnf("path does not exist on disk! Run `luban setup --only {} --force`?",
                   hit->component);
        return 1;
    }
    return 0;
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
        log::info("install it via:  luban setup --only vcpkg");
        return 1;
    }

    std::vector<std::string> cmd = {vcpkg->exe.string(), "search", pattern};
    auto env_overrides = env_snapshot::apply_to({});
    return proc::run(cmd, fs::current_path().string(), env_overrides);
}

}  // namespace

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
        "  Requires vcpkg to be installed (luban setup --only vcpkg).\n"
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
