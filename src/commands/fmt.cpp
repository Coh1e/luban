// `luban fmt` — clang-format -i over project source files.
//
// Walks <project>/{src,include,tests}/ recursively, runs clang-format on
// every C/C++ source file. Respects the nearest .clang-format. Skips any
// path containing `third_party`. With --check, prints diffs and exits
// non-zero (CI-friendly).

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

std::string clang_format_exe() {
    auto recs = registry::load_installed();
    for (auto* comp : {"llvm-mingw", "clang"}) {
        auto it = recs.find(comp);
        if (it == recs.end()) continue;
        fs::path root = paths::toolchain_dir(it->second.toolchain_dir);
        for (auto& [alias, rel] : it->second.bins) {
            if (alias == "clang-format") {
                std::string norm = rel;
                for (auto& c : norm) {
                    if (c == '/' || c == '\\') c = static_cast<char>(fs::path::preferred_separator);
                }
                return (root / norm).string();
            }
        }
    }
    fs::path shim = paths::bin_dir() / "clang-format.cmd";
    std::error_code ec;
    if (fs::exists(shim, ec)) return shim.string();
    return "clang-format";
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

bool is_cxx_source(const fs::path& p) {
    static const std::vector<std::string> exts = {".h", ".hpp", ".hh", ".c", ".cc", ".cpp", ".cxx"};
    std::string ext = p.extension().string();
    for (auto& e : exts) if (ext == e) return true;
    return false;
}

std::vector<fs::path> collect_sources(const fs::path& root) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (auto sub : {"src", "include", "tests"}) {
        fs::path dir = root / sub;
        if (!fs::exists(dir, ec)) continue;
        for (auto it = fs::recursive_directory_iterator(dir, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            // Skip third_party paths anywhere deep
            std::string s = it->path().string();
            if (s.find("third_party") != std::string::npos) continue;
            if (it->is_regular_file(ec) && is_cxx_source(it->path())) {
                out.push_back(it->path());
            }
        }
    }
    return out;
}

int run_fmt(const cli::ParsedArgs& a) {
    fs::path project = find_project_root();
    if (project.empty()) {
        log::err("no CMakeLists.txt up from cwd");
        return 2;
    }
    bool check = a.flags.count("check") && a.flags.at("check");

    auto files = collect_sources(project);
    if (files.empty()) {
        log::infof("no C/C++ sources under {}/{{src,include,tests}}", project.string());
        return 0;
    }

    auto env_overrides = env_snapshot::apply_to({});
    std::string cf = clang_format_exe();

    std::vector<std::string> cmd;
    cmd.reserve(files.size() + 4);
    cmd.push_back(cf);
    if (check) {
        cmd.push_back("--dry-run");
        cmd.push_back("--Werror");
    } else {
        cmd.push_back("-i");
    }
    cmd.push_back("--style=file");
    for (auto& f : files) cmd.push_back(f.string());

    log::stepf("{} {} files", check ? "checking" : "formatting", files.size());
    int rc = proc::run(cmd, project.string(), env_overrides);
    if (rc != 0) {
        if (check) log::warn("formatting differences detected (run `luban fmt` to apply)");
        else log::errf("clang-format returned {}", rc);
        return rc;
    }
    log::ok(check ? "formatting clean" : "formatted");
    return 0;
}

}  // namespace

void register_fmt() {
    cli::Subcommand c;
    c.name = "fmt";
    c.help = "clang-format -i over src/include/tests (respects .clang-format)";
    c.group = "project";
    c.long_help =
        "  Walks <project>/{src,include,tests}/ recursively and runs\n"
        "  clang-format -i on every .h/.hpp/.c/.cc/.cpp/.cxx file. Respects\n"
        "  the project's .clang-format. Skips any path containing 'third_party'.\n"
        "\n"
        "  --check  Don't modify files; print diffs and exit non-zero if any\n"
        "           file would change. Suitable for CI gating.";
    c.flags = {"check"};
    c.examples = {
        "luban fmt\tFormat in place",
        "luban fmt --check\tCI mode: report diffs without writing",
    };
    c.run = run_fmt;
    cli::register_subcommand(std::move(c));
}

}  // namespace luban::commands
