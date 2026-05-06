// `luban check` — cmake configure with -Wdev (no build).
//
// Fast preflight that catches CMakeLists.txt / preset / toolchain
// misconfigurations before committing to a full build. Reuses the
// preset auto-pick logic from `luban build`.

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

std::string cmake_exe() {
    auto recs = registry::load_installed();
    auto it = recs.find("cmake");
    if (it != recs.end()) {
        fs::path root = paths::toolchain_dir(it->second.toolchain_dir);
        for (auto& [alias, rel] : it->second.bins) {
            if (alias == "cmake") {
                std::string norm = rel;
                for (auto& c : norm) {
                    if (c == '/' || c == '\\') c = static_cast<char>(fs::path::preferred_separator);
                }
                return (root / norm).string();
            }
        }
    }
    fs::path shim = paths::bin_dir() / "cmake.cmd";
    std::error_code ec;
    if (fs::exists(shim, ec)) return shim.string();
    return "cmake";
}

fs::path find_project_root() {
    std::error_code ec;
    fs::path d = fs::current_path(ec);
    while (!d.empty()) {
        if (fs::exists(d / "CMakeLists.txt", ec)) return d;
        fs::path parent = d.parent_path();
        if (parent == d) break;
        d = parent;
    }
    return {};
}

std::string pick_preset(const fs::path& project, const std::string& explicit_preset) {
    if (!explicit_preset.empty() && explicit_preset != "auto") return explicit_preset;
    fs::path vcpkg_json = project / "vcpkg.json";
    std::error_code ec;
    if (!fs::exists(vcpkg_json, ec)) return "default";
    auto m = vcpkg_manifest::load(vcpkg_json);
    return m.dependencies.empty() ? "no-vcpkg" : "default";
}

int run_check(const cli::ParsedArgs& a) {
    fs::path project = find_project_root();
    if (project.empty()) {
        log::err("no CMakeLists.txt up from cwd");
        return 2;
    }

    std::string preset_raw = a.opts.count("preset") ? a.opts.at("preset") : std::string("auto");
    std::string preset = pick_preset(project, preset_raw);
    if (preset_raw == "auto") log::infof("preset: {} (auto-picked)", preset);

    auto env_overrides = env_snapshot::apply_to({});
    std::string cmake = cmake_exe();
    std::vector<std::string> cmd = {cmake, "--preset", preset, "-Wdev"};
    log::stepf("{} --preset {} -Wdev", cmake, preset);
    int rc = proc::run(cmd, project.string(), env_overrides);
    if (rc != 0) {
        log::errf("cmake configure returned {}", rc);
        return rc;
    }
    log::ok("configure clean");
    return 0;
}

}  // namespace

void register_check() {
    cli::Subcommand c;
    c.name = "check";
    c.help = "cmake configure with -Wdev (no build)";
    c.group = "project";
    c.long_help =
        "  Runs `cmake --preset <P> -Wdev` to validate the project's\n"
        "  configuration without spending time on the full build. Catches\n"
        "  CMakeLists.txt / preset / toolchain mistakes early.\n"
        "\n"
        "  Preset auto-selection (default --preset=auto) matches `luban build`:\n"
        "    vcpkg.json has deps   -> preset 'default' (uses VCPKG_ROOT)\n"
        "    vcpkg.json deps empty -> preset 'no-vcpkg'";
    c.opts = {{"preset", "auto"}};
    c.examples = {
        "luban check\tConfigure with auto-picked preset",
        "luban check --preset release\tConfigure with release preset",
    };
    c.run = run_check;
    cli::register_subcommand(std::move(c));
}

}  // namespace luban::commands
