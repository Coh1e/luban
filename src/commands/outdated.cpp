// `luban outdated` — report installed deps lagging behind the registry.
//
// Wraps `vcpkg x-update-baseline --dry-run`, which prints the diff
// between the project's pinned baseline and the registry's current HEAD
// without writing any files. To actually advance the baseline, use
// `luban update`.

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

int run_outdated(const cli::ParsedArgs& /*a*/) {
    fs::path project = find_project_root();
    if (project.empty()) {
        log::err("no vcpkg.json up from cwd");
        return 2;
    }

    auto env_overrides = env_snapshot::apply_to({});
    std::string vcpkg = vcpkg_exe();
    std::vector<std::string> cmd = {vcpkg, "x-update-baseline", "--dry-run"};
    log::stepf("{} x-update-baseline --dry-run", vcpkg);
    int rc = proc::run(cmd, project.string(), env_overrides);
    if (rc != 0) {
        log::errf("vcpkg x-update-baseline --dry-run returned {}", rc);
    }
    return rc;
}

}  // namespace

void register_outdated() {
    cli::Subcommand c;
    c.name = "outdated";
    c.help = "report deps lagging behind registry baseline (vcpkg x-update-baseline --dry-run)";
    c.group = "project";
    c.long_help =
        "  Runs `vcpkg x-update-baseline --dry-run`, which prints the diff\n"
        "  between the project's pinned baseline and the vcpkg registry's\n"
        "  current HEAD without writing any files.\n"
        "\n"
        "  To actually advance the project's baseline, use `luban update`.";
    c.examples = {
        "luban outdated\tShow what would change if you ran `luban update`",
    };
    c.run = run_outdated;
    cli::register_subcommand(std::move(c));
}

}  // namespace luban::commands
