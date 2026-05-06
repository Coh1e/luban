// `luban test` — wraps ctest against an existing build dir.
//
// Runs `ctest --test-dir build/<preset> --output-on-failure`. Does NOT
// auto-build; user runs `luban build` first. Detects CTestTestfile
// presence so projects without add_test() exit 0 with an info line
// rather than ctest's confusing "No tests were found!!! exit code 8".

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

std::string ctest_exe() {
    auto recs = registry::load_installed();
    auto it = recs.find("cmake");
    if (it != recs.end()) {
        fs::path root = paths::toolchain_dir(it->second.toolchain_dir);
        for (auto& [alias, rel] : it->second.bins) {
            if (alias == "ctest") {
                std::string norm = rel;
                for (auto& c : norm) {
                    if (c == '/' || c == '\\') c = static_cast<char>(fs::path::preferred_separator);
                }
                return (root / norm).string();
            }
        }
    }
    fs::path shim = paths::bin_dir() / "ctest.cmd";
    std::error_code ec;
    if (fs::exists(shim, ec)) return shim.string();
    return "ctest";
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

int run_test(const cli::ParsedArgs& a) {
    fs::path project = find_project_root();
    if (project.empty()) {
        log::err("no CMakeLists.txt up from cwd");
        return 2;
    }

    std::string preset = a.opts.count("preset") ? a.opts.at("preset") : std::string("default");
    fs::path build_dir = project / "build" / preset;
    std::error_code ec;
    if (!fs::exists(build_dir, ec)) {
        log::warnf("build/{} does not exist - run `luban build --preset {}` first", preset, preset);
        return 2;
    }

    fs::path ctest_file = build_dir / "CTestTestfile.cmake";
    if (!fs::exists(ctest_file, ec)) {
        log::infof("no CTestTestfile in build/{}/ - re-run `luban build` to refresh", preset);
        return 0;
    }

    auto env_overrides = env_snapshot::apply_to({});
    std::string ctest = ctest_exe();
    std::vector<std::string> cmd = {ctest, "--test-dir", build_dir.string(), "--output-on-failure"};
    log::stepf("{} --test-dir build/{} --output-on-failure", ctest, preset);
    int rc = proc::run(cmd, project.string(), env_overrides);
    // ctest exit 8 = "no tests were found"; treat as info, not error.
    if (rc == 8) {
        log::info("ctest reported no tests defined");
        return 0;
    }
    if (rc != 0) {
        log::errf("ctest returned {}", rc);
        return rc;
    }
    log::ok("all tests passed");
    return 0;
}

}  // namespace

void register_test() {
    cli::Subcommand c;
    c.name = "test";
    c.help = "run ctest against build/<preset>/";
    c.group = "project";
    c.long_help =
        "  Runs `ctest --test-dir build/<preset> --output-on-failure` against\n"
        "  an already-configured build directory. Does not auto-build - run\n"
        "  `luban build` first.\n"
        "\n"
        "  Projects without add_test() calls exit 0 with an info message;\n"
        "  this verb won't surface ctest's 'No tests were found' as an error.";
    c.opts = {{"preset", "default"}};
    c.examples = {
        "luban test\tRun ctest on build/default/",
        "luban test --preset release\tRun ctest on build/release/",
    };
    c.run = run_test;
    cli::register_subcommand(std::move(c));
}

}  // namespace luban::commands
