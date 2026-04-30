// `luban shim` — regenerate every shim in bin_dir() from installed.json.
//
// Two artifacts written per managed alias:
//
//   1. <alias>.cmd — a tiny CMD batch that exec's the real exe with the
//      original argv. Works in interactive shells (cmd, pwsh both run .cmd).
//
//   2. <alias>.exe — a hardlink (or copy fallback) of luban-shim.exe. cmake's
//      compiler probe + IDE tooling iterate PATH looking for literal .exe
//      files, so the .cmd alone wasn't enough; the .exe twin makes aliases
//      first-class for those consumers. luban-shim.exe at runtime reads
//      <bin_dir>/.shim-table.json (sibling lookup) to translate its hardlinked
//      alias back into a target exe path.
//
// Earlier versions also wrote .ps1 / extensionless .sh; dropped to keep
// ~/.local/bin tidy after the migration to that shared XDG bin home.
//
// Collision policy: bin_dir is shared (uv/pipx/claude-code etc. install into
// the same `~/.local/bin`). For each alias, we check `.shim-table.json`
// before writing. Files we don't own get left alone (warn + skip) unless
// `--force` is passed. The table is also our authority for cleanup on
// uninstall — only entries we wrote get removed.

#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

#include "json.hpp"

#include "../cli.hpp"
#include "../log.hpp"
#include "../paths.hpp"
#include "../registry.hpp"
#include "../shim.hpp"

namespace luban::commands {

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

fs::path table_file_path() { return paths::bin_dir() / ".shim-table.json"; }

// Locate the luban-shim.exe template, in priority order:
//   1. paths::bin_dir() / "luban-shim.exe"       — preferred, install.ps1
//      drops it here at first install. All hardlinks within bin_dir are
//      same-volume, so the hardlink path is always taken (no copy fallback).
//   2. <luban.exe dir>/luban-shim.exe            — when luban.exe and shim
//      live alongside one another (build output, dev runs).
//   3. <luban.exe dir>/../bin/luban-shim.exe     — uncommon, kept for compat.
//   4. <luban.exe dir>/../build/{release,default}/luban-shim.exe — dev tree.
fs::path find_luban_shim_template() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4];
    DWORD got = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    fs::path self = (got == 0) ? fs::current_path() / "luban.exe" : fs::path(std::wstring(buf, got));
#else
    fs::path self = fs::current_path() / "luban";
#endif
    fs::path d = self.parent_path();
    std::vector<fs::path> candidates = {
        paths::bin_dir() / "luban-shim.exe",
        d / "luban-shim.exe",
        d.parent_path() / "bin" / "luban-shim.exe",
        d.parent_path() / "build" / "release" / "luban-shim.exe",
        d.parent_path() / "build" / "default" / "luban-shim.exe",
    };
    std::error_code ec;
    for (auto& c : candidates) {
        if (fs::exists(c, ec)) return c;
    }
    return {};
}

// Ensure paths::bin_dir() has a luban-shim.exe to hardlink from. If template
// was found elsewhere (e.g. luban.exe sibling at first install), copy it into
// bin_dir so subsequent hardlinks are guaranteed same-volume. Returns the
// canonical bin_dir path; falls back to original template on copy failure.
fs::path ensure_template_in_bin(const fs::path& template_exe) {
    fs::path target = paths::bin_dir() / "luban-shim.exe";
    std::error_code ec;
    if (fs::exists(target, ec)) return target;
    fs::create_directories(target.parent_path(), ec);
    fs::copy_file(template_exe, target, fs::copy_options::overwrite_existing, ec);
    if (ec) return template_exe;  // hardlinks may need cross-volume copy fallback
    return target;
}

// Read existing table. Used to know which aliases we currently own (for the
// collision-detection step) and to know which orphan entries to clean up.
std::map<std::string, std::string> load_table() {
    std::map<std::string, std::string> out;
    std::error_code ec;
    fs::path p = table_file_path();
    if (!fs::exists(p, ec)) return out;
    std::ifstream in(p, std::ios::binary);
    if (!in) return out;
    json doc;
    try { in >> doc; } catch (...) { return out; }
    if (!doc.is_object()) return out;
    for (auto& [k, v] : doc.items()) {
        if (v.is_string()) out[k] = v.get<std::string>();
    }
    return out;
}

// Hard-link <bin>/<alias>.exe → luban-shim.exe template. Falls back to copy
// if hardlink fails (cross-volume — should be rare since template lives in
// bin_dir; or filesystem doesn't support hardlinks).
//
// `previously_managed`: aliases we wrote in a prior run (read from on-disk
// table at run start). If <alias>.exe exists but its alias isn't in this set
// AND `force` is false, refuse to clobber — that .exe belongs to someone else.
bool write_exe_shim(const std::string& alias,
                    const fs::path& template_exe,
                    const std::set<std::string>& previously_managed,
                    bool force) {
    fs::path dst = paths::bin_dir() / (alias + ".exe");
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);

    if (fs::exists(dst, ec)) {
        bool ours = previously_managed.count(alias) > 0;
        if (!ours && !force) {
            return false;  // collision — caller logs
        }
        fs::remove(dst, ec);
    }

#ifdef _WIN32
    // Hardlink path: zero extra disk, same inode, atomic for our purposes.
    if (CreateHardLinkW(dst.wstring().c_str(), template_exe.wstring().c_str(), nullptr)) {
        return true;
    }
#else
    fs::create_hard_link(template_exe, dst, ec);
    if (!ec) return true;
    ec.clear();
#endif

    // Fallback: copy the binary. Costs ~200 KB per alias but works across
    // volumes / non-hardlink filesystems.
    fs::copy_file(template_exe, dst, fs::copy_options::overwrite_existing, ec);
    return !ec;
}

// Write .shim-table.json atomically. Replaces the file with `new_table`;
// caller is responsible for removing orphan files for entries that disappeared
// since the previous run (we do that in run_shim).
void write_shim_table(const std::map<std::string, fs::path>& alias_to_exe) {
    json table = json::object();
    for (auto& [alias, exe] : alias_to_exe) {
        table[alias] = exe.string();
    }
    fs::path out = table_file_path();
    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    fs::path tmp = out; tmp += ".tmp";
    {
        std::ofstream of(tmp, std::ios::binary | std::ios::trunc);
        of << table.dump(2) << '\n';
    }
    fs::rename(tmp, out, ec);
    if (ec) {
        // Cross-volume rename can fail on some Windows setups; copy + delete
        // gets the same effect.
        fs::copy_file(tmp, out, fs::copy_options::overwrite_existing, ec);
        fs::remove(tmp, ec);
    }
}

int run_shim(const cli::ParsedArgs& args) {
    bool force = args.flags.count("force") && args.flags.at("force");

    auto recs = registry::load_installed();
    if (recs.empty()) {
        log::warn("registry is empty \xe2\x80\x94 nothing to shim. Run setup first.");
        return 0;
    }

    fs::path tpl_initial = find_luban_shim_template();
    bool have_exe_shim = !tpl_initial.empty();
    fs::path tpl;
    if (have_exe_shim) {
        tpl = ensure_template_in_bin(tpl_initial);
    } else {
        log::warn("luban-shim.exe template not found; writing .cmd shims only");
        log::info("(build the shim_exe target and place luban-shim.exe alongside luban.exe)");
    }

    // Aliases we owned in the previous run — used by collision detection.
    auto old_table = load_table();
    std::set<std::string> previously_managed;
    for (auto& [k, _] : old_table) previously_managed.insert(k);

    std::map<std::string, fs::path> new_table;
    std::vector<std::string> collisions;
    int text_count = 0;
    int exe_count = 0;

    for (auto& [name, rec] : recs) {
        fs::path root = paths::toolchain_dir(rec.toolchain_dir);
        for (auto& [alias, rel] : rec.bins) {
            // bins paths in installed.json may use either separator; normalize.
            std::string norm = rel;
            for (auto& c : norm) if (c == '/' || c == '\\') c = static_cast<char>(fs::path::preferred_separator);
            fs::path exe = root / norm;

            // Text shim (.cmd). shim::write_shim does its own collision check
            // via the on-disk table; force=true short-circuits.
            auto wr = shim::write_shim(alias, exe, {}, force);
            if (wr == shim::WriteResult::Wrote) {
                ++text_count;
                // Record ownership as soon as ANY shim (.cmd or .exe) lands —
                // otherwise next run won't recognize the alias as ours and
                // will treat its files as collisions.
                new_table[alias] = exe;
            } else if (wr == shim::WriteResult::Skipped) {
                collisions.push_back(alias + ".cmd");
                continue;  // also skip the .exe twin to keep behavior consistent
            }

            if (have_exe_shim) {
                if (write_exe_shim(alias, tpl, previously_managed, force)) {
                    ++exe_count;
                    // Already in new_table from the .cmd success above.
                } else {
                    collisions.push_back(alias + ".exe");
                }
            }
        }
    }

    // Clean up orphan entries: aliases we owned but are no longer in
    // installed.json (uninstalled component). Their files in bin_dir are ours
    // to delete since the table guaranteed ownership.
    int orphans = 0;
    for (auto& [old_alias, _] : old_table) {
        if (new_table.find(old_alias) == new_table.end()) {
            orphans += shim::remove_shim(old_alias);
        }
    }

    write_shim_table(new_table);

    log::okf(".cmd shims: {} ({})", text_count, paths::bin_dir().string());
    if (have_exe_shim) {
        log::okf(".exe shims: {} (hardlinks to luban-shim.exe)", exe_count);
    }
    log::okf("shim table:  {}", table_file_path().string());
    if (orphans > 0) {
        log::infof("cleaned {} orphan shim file(s) from removed components", orphans);
    }
    if (!collisions.empty()) {
        log::warnf("{} shim(s) skipped due to collisions with non-luban files:", collisions.size());
        for (auto& c : collisions) log::warnf("  {}", c);
        log::info("(re-run with --force to overwrite, or remove the conflicting files manually)");
    }
    return 0;
}

}  // namespace

void register_shim() {
    cli::Subcommand c;
    c.name = "shim";
    c.help = "regenerate ~/.local/bin shims from installed.json";
    c.group = "advanced";
    c.long_help =
        "  Walk every component in installed.json and rewrite shims under\n"
        "  bin_dir() (~/.local/bin by default — XDG-shared with uv/pipx/etc).\n"
        "  Two artifacts per alias:\n"
        "\n"
        "    1. <alias>.cmd      — text shim, works in any interactive shell\n"
        "    2. <alias>.exe      — hardlink to luban-shim.exe; cmake/clangd/\n"
        "                          IDE tooling treat aliases as real exes.\n"
        "                          Sibling .shim-table.json holds alias→exe map.\n"
        "\n"
        "  Collision policy: bin_dir is shared. We use .shim-table.json to\n"
        "  remember which files we wrote. Files we don't own are skipped (a\n"
        "  warning is printed). Pass --force to clobber non-luban files.\n"
        "\n"
        "  Run when: shim dir got deleted, toolchain dir moved, or after\n"
        "  upgrading luban itself (shim format may have changed).";
    c.flags = {"force"};
    c.examples = {
        "luban shim\tRepair all shims (skip collisions)",
        "luban shim --force\tOverwrite even non-luban files in bin_dir",
    };
    c.run = run_shim;
    cli::register_subcommand(std::move(c));
}

}  // namespace luban::commands
