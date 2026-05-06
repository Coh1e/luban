// `luban update` — bump vcpkg builtin-baseline to current registry HEAD.
//
// Default: dry-run (DESIGN §16; matches cargo `update --dry-run` / npm
// `outdated` semantics — non-destructive by default). Pass `--apply` to
// rewrite the project's vcpkg.json / vcpkg-configuration.json baseline
// to the current registry HEAD.
//
// Wraps `vcpkg x-update-baseline [--dry-run | --add-initial-baseline]`.
// Idempotent in either mode.

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

int run_update(const cli::ParsedArgs& a) {
    fs::path project = find_project_root();
    if (project.empty()) {
        log::err("no vcpkg.json up from cwd");
        return 2;
    }

    bool apply = a.flags.count("apply") && a.flags.at("apply");

    auto env_overrides = env_snapshot::apply_to({});
    std::string vcpkg = vcpkg_exe();
    std::vector<std::string> cmd;
    if (apply) {
        cmd = {vcpkg, "x-update-baseline", "--add-initial-baseline"};
        log::stepf("{} x-update-baseline --add-initial-baseline", vcpkg);
    } else {
        cmd = {vcpkg, "x-update-baseline", "--dry-run"};
        log::stepf("{} x-update-baseline --dry-run (rerun with --apply to commit)", vcpkg);
    }
    int rc = proc::run(cmd, project.string(), env_overrides);
    if (rc != 0) {
        log::errf("vcpkg x-update-baseline returned {}", rc);
        return rc;
    }
    if (apply) {
        log::ok("baseline updated; next `luban build` will pick up newer port versions");
    } else {
        log::info("dry-run complete; rerun with --apply to commit baseline change");
    }
    return 0;
}

}  // namespace

void register_update() {
    cli::Subcommand c;
    c.name = "update";
    c.help = "bump vcpkg builtin-baseline to current registry HEAD (dry-run by default)";
    c.group = "project";
    c.long_help =
        "  Reports (default) or applies (--apply) a bump of the project's\n"
        "  vcpkg builtin-baseline to the current vcpkg registry HEAD.\n"
        "\n"
        "  Default mode runs `vcpkg x-update-baseline --dry-run` — non-destructive\n"
        "  preview of what would change. Pass `--apply` to actually rewrite\n"
        "  vcpkg.json / vcpkg-configuration.json (idempotent).\n"
        "\n"
        "  See also: `luban outdated` (alias for the dry-run path).";
    c.flags = {"apply"};
    c.examples = {
        "luban update\tDry-run: show what would change",
        "luban update --apply\tCommit baseline bump + re-resolve next build",
    };
    c.run = run_update;
    cli::register_subcommand(std::move(c));
}

}  // namespace luban::commands
