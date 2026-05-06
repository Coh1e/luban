// `luban clean` — remove build artifacts.
//
// Scopes (DESIGN §16):
//   default       <project>/build/
//   --vcpkg       additionally <project>/vcpkg_installed/ + compile_commands.json
//   --cache       additionally luban's own download cache (<cache>/downloads/)
//
// Refuses to run if cwd has no project sentinel (vcpkg.json or
// CMakeLists.txt) so a stray invocation can't nuke a random directory.
// `--cache` is the only scope that touches paths outside the project.

#include <filesystem>
#include <system_error>

#include "../cli.hpp"
#include "../log.hpp"
#include "../paths.hpp"

namespace luban::commands {
namespace {

namespace fs = std::filesystem;

fs::path find_project_root() {
    std::error_code ec;
    fs::path d = fs::current_path(ec);
    while (!d.empty()) {
        if (fs::exists(d / "vcpkg.json", ec) || fs::exists(d / "CMakeLists.txt", ec)) {
            return d;
        }
        fs::path parent = d.parent_path();
        if (parent == d) break;
        d = parent;
    }
    return {};
}

enum class RemoveResult { NotPresent, Removed, Failed };

RemoveResult remove_if_present(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(p, ec)) return RemoveResult::NotPresent;
    fs::remove_all(p, ec);
    if (ec) {
        log::errf("failed to remove {}: {}", p.string(), ec.message());
        return RemoveResult::Failed;
    }
    log::okf("removed {}", p.string());
    return RemoveResult::Removed;
}

int run_clean(const cli::ParsedArgs& a) {
    fs::path root = find_project_root();
    if (root.empty()) {
        log::err("no project root (no vcpkg.json or CMakeLists.txt up from cwd) - refusing to clean");
        return 2;
    }

    bool wipe_vcpkg = a.flags.count("vcpkg") && a.flags.at("vcpkg");
    bool wipe_cache = a.flags.count("cache") && a.flags.at("cache");

    int removed = 0, failed = 0;
    auto bump = [&](RemoveResult r) {
        if (r == RemoveResult::Removed) ++removed;
        if (r == RemoveResult::Failed) ++failed;
    };
    bump(remove_if_present(root / "build"));
    if (wipe_vcpkg) {
        bump(remove_if_present(root / "vcpkg_installed"));
        bump(remove_if_present(root / "compile_commands.json"));
    }
    if (wipe_cache) {
        bump(remove_if_present(paths::cache_dir() / "downloads"));
    }
    if (failed) return 1;
    if (removed == 0) log::infof("nothing to clean in {}", root.string());
    return 0;
}

}  // namespace

void register_clean() {
    cli::Subcommand c;
    c.name = "clean";
    c.help = "remove build/ (with --vcpkg / --cache to widen scope)";
    c.group = "project";
    c.long_help =
        "  Removes <project>/build/. Refuses to run outside a project root\n"
        "  (no vcpkg.json + no CMakeLists.txt) so stray invocations can't\n"
        "  nuke a random directory.\n"
        "\n"
        "  --vcpkg  Also remove <project>/vcpkg_installed/ and the synced\n"
        "           compile_commands.json. Use when switching toolchains or\n"
        "           after a vcpkg baseline bump.\n"
        "  --cache  Also remove luban's download cache (<cache>/downloads/).\n"
        "           Frees disk after large blueprint applies (llvm-mingw,\n"
        "           cmake archives etc.); next bp apply re-downloads.\n"
        "\n"
        "  Flags can combine: `luban clean --vcpkg --cache`.";
    c.flags = {"vcpkg", "cache"};
    c.examples = {
        "luban clean\tRemove build/",
        "luban clean --vcpkg\tAlso remove vcpkg_installed/ + compile_commands.json",
        "luban clean --cache\tAlso remove luban download cache",
        "luban clean --vcpkg --cache\tNuke build, vcpkg, and download cache",
    };
    c.run = run_clean;
    cli::register_subcommand(std::move(c));
}

}  // namespace luban::commands
