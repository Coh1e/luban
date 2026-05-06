// `luban tree` — show vcpkg dependency tree for the project.
//
// Wraps `vcpkg depend-info --format=tree <pkg>...` for each direct dep
// in vcpkg.json. Manifest mode only (vcpkg.json must exist).

#include <filesystem>
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
namespace {

namespace fs = std::filesystem;

std::string vcpkg_exe() {
    auto recs = registry::load_installed();
    auto it = recs.find("vcpkg");
    if (it != recs.end()) {
        fs::path root = paths::toolchain_dir(it->second.toolchain_dir);
        for (auto& [alias, rel] : it->second.bins) {
            if (alias == "vcpkg") {
                std::string norm = rel;
                for (auto& c : norm) {
                    if (c == '/' || c == '\\') c = static_cast<char>(fs::path::preferred_separator);
                }
                return (root / norm).string();
            }
        }
    }
    fs::path shim = paths::bin_dir() / "vcpkg.cmd";
    std::error_code ec;
    if (fs::exists(shim, ec)) return shim.string();
    return "vcpkg";
}

fs::path find_project_root() {
    std::error_code ec;
    fs::path d = fs::current_path(ec);
    while (!d.empty()) {
        if (fs::exists(d / "vcpkg.json", ec)) return d;
        fs::path parent = d.parent_path();
        if (parent == d) break;
        d = parent;
    }
    return {};
}

int run_tree(const cli::ParsedArgs& /*a*/) {
    fs::path project = find_project_root();
    if (project.empty()) {
        log::err("no vcpkg.json up from cwd");
        return 2;
    }
    auto m = vcpkg_manifest::load(project / "vcpkg.json");
    if (m.dependencies.empty()) {
        log::info("vcpkg.json has no dependencies");
        return 0;
    }

    auto env_overrides = env_snapshot::apply_to({});
    std::string vcpkg = vcpkg_exe();

    std::vector<std::string> cmd = {vcpkg, "depend-info", "--format=tree"};
    for (auto& d : m.dependencies) cmd.push_back(d.name);

    log::stepf("{} depend-info --format=tree ({} pkgs)", vcpkg, m.dependencies.size());
    int rc = proc::run(cmd, project.string(), env_overrides);
    if (rc != 0) {
        log::errf("vcpkg depend-info returned {}", rc);
        log::info("tip: ensure VCPKG_ROOT is set and vcpkg is on PATH");
    }
    return rc;
}

}  // namespace

void register_tree() {
    cli::Subcommand c;
    c.name = "tree";
    c.help = "show vcpkg dependency tree (vcpkg depend-info)";
    c.group = "project";
    c.long_help =
        "  For each direct dependency in vcpkg.json, asks vcpkg for its\n"
        "  transitive dependency tree (`vcpkg depend-info --format=tree`).\n"
        "\n"
        "  Requires vcpkg to be installed and VCPKG_ROOT set.";
    c.examples = {
        "luban tree\tASCII tree of all direct deps",
    };
    c.run = run_tree;
    cli::register_subcommand(std::move(c));
}

}  // namespace luban::commands
