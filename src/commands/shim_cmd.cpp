// `luban shim` — regenerate every shim under <data>/bin/ from the registry.
//
// Two layers of shim, both written each time:
//
//   1. **.cmd / .ps1 / extensionless sh** — text shims, work for shell users.
//      Cheap, debuggable, but cmake's compiler probe doesn't accept them.
//
//   2. **<alias>.exe** (rustup-style native proxy) — hard-linked to a single
//      luban-shim.exe binary, plus a sibling `.shim-table.json` with alias→exe
//      target paths. Lets cmake / clangd / IDE tooling treat aliases as real
//      executables.
//
// Both layers coexist; PATHEXT prefers .exe on Windows so the .exe shim wins
// for tools that use SearchPathW. Shells (cmd, pwsh) still get .cmd / .ps1.

#include <filesystem>
#include <fstream>
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

// Locate the luban-shim.exe template. Three candidates (priority order):
//   1. <luban.exe dir>/luban-shim.exe       (installed alongside)
//   2. <luban.exe dir>/../bin/luban-shim.exe (uncommon)
//   3. <luban.exe dir>/luban-shim.exe via build/{default,release} dir
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

// Hard-link <bin>/<alias>.exe → luban-shim.exe template. Falls back to copy
// if hardlink fails (cross-volume, FS doesn't support, etc.).
bool write_exe_shim(const std::string& alias, const fs::path& template_exe) {
    fs::path dst = paths::bin_dir() / (alias + ".exe");
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);

    if (fs::exists(dst, ec)) fs::remove(dst, ec);

#ifdef _WIN32
    // Try hardlink first (same volume → 0 bytes extra disk).
    if (CreateHardLinkW(dst.wstring().c_str(), template_exe.wstring().c_str(), nullptr)) {
        return true;
    }
#else
    fs::create_hard_link(template_exe, dst, ec);
    if (!ec) return true;
    ec.clear();
#endif

    // Fallback: copy (cross-volume / unsupported FS).
    fs::copy_file(template_exe, dst, fs::copy_options::overwrite_existing, ec);
    return !ec;
}

void write_shim_table(const std::map<std::string, fs::path>& alias_to_exe) {
    json table = json::object();
    for (auto& [alias, exe] : alias_to_exe) {
        table[alias] = exe.string();
    }
    fs::path out = paths::bin_dir() / ".shim-table.json";
    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    fs::path tmp = out; tmp += ".tmp";
    {
        std::ofstream of(tmp, std::ios::binary | std::ios::trunc);
        of << table.dump(2) << '\n';
    }
    fs::rename(tmp, out, ec);
    if (ec) {
        fs::copy_file(tmp, out, fs::copy_options::overwrite_existing, ec);
        fs::remove(tmp, ec);
    }
}

int run_shim(const cli::ParsedArgs&) {
    auto recs = registry::load_installed();
    if (recs.empty()) {
        log::warn("registry is empty \xe2\x80\x94 nothing to shim. Run setup first.");
        return 0;
    }

    fs::path tpl = find_luban_shim_template();
    bool have_exe_shim = !tpl.empty();
    if (!have_exe_shim) {
        log::warn("luban-shim.exe template not found; only writing .cmd/.ps1/sh shims");
        log::info("(rebuild luban with the cmake target and place luban-shim.exe alongside luban.exe)");
    }

    std::map<std::string, fs::path> table;
    int text_count = 0;
    int exe_count = 0;
    for (auto& [name, rec] : recs) {
        fs::path root = paths::toolchain_dir(rec.toolchain_dir);
        for (auto& [alias, rel] : rec.bins) {
            std::string norm = rel;
            for (auto& c : norm) if (c == '/' || c == '\\') c = static_cast<char>(fs::path::preferred_separator);
            fs::path exe = root / norm;

            // text shims (always)
            shim::write_shim(alias, exe);
            ++text_count;

            // exe shim (when template available)
            if (have_exe_shim) {
                if (write_exe_shim(alias, tpl)) {
                    ++exe_count;
                    table[alias] = exe;
                }
            }
        }
    }
    if (have_exe_shim) write_shim_table(table);

    log::okf("text shims: {} (.cmd/.ps1/sh under {})",
             text_count, paths::bin_dir().string());
    if (have_exe_shim) {
        log::okf("exe shims: {} (rustup-style hard-linked to luban-shim.exe)", exe_count);
        log::okf("shim table: {}", (paths::bin_dir() / ".shim-table.json").string());
    }
    return 0;
}

}  // namespace

void register_shim() {
    cli::Subcommand c;
    c.name = "shim";
    c.help = "regenerate <data>/bin/ shims from installed.json (text + .exe)";
    c.group = "advanced";
    c.long_help =
        "  Walk every component in installed.json and rewrite shims under\n"
        "  <data>/bin/. Two flavors written:\n"
        "\n"
        "    1. text shims (.cmd, .ps1, extensionless sh) — for shell users.\n"
        "    2. .exe shims (rustup-style) — hard-linked to luban-shim.exe;\n"
        "       cmake compiler probe + IDEs see them as real executables.\n"
        "       Sibling .shim-table.json maps alias → target exe; luban-shim\n"
        "       reads it at runtime.\n"
        "\n"
        "  Run when: shim dir got deleted, toolchain dir moved, or after\n"
        "  upgrading luban itself (shim format may have changed).";
    c.examples = {
        "luban shim\tRepair all shims",
    };
    c.run = run_shim;
    cli::register_subcommand(std::move(c));
}

}  // namespace luban::commands
