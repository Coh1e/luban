// `luban env` — display environment state, register on HKCU (--user), or
// emit eval-able env exports for the requested shell (--print).
//
// The `--apply` activate-scripts mode was removed in v0.2; --print covers
// the same use case (CI / containers / one-off shells) without leaving
// stale .cmd/.ps1/.sh files on disk.
//
// LUBAN_DATA/CACHE/STATE/CONFIG HKCU writes were also removed: they had
// zero on-disk consumers and only created visual clutter in
// HKCU\Environment. Today HKCU writes are limited to PATH + VCPKG_ROOT
// (+ EM_CONFIG when emscripten is installed).

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "../cli.hpp"
#include "../env_snapshot.hpp"
#include "../log.hpp"
#include "../paths.hpp"
#include "../registry.hpp"
#include "../win_path.hpp"

namespace luban::commands {

namespace {

namespace fs = std::filesystem;

// Legacy HKCU env vars luban used to write but no longer does. We still
// unset them in --unset-user so users upgrading from v0.1.x get a clean
// slate without manual reg cleanup.
constexpr const char* kLegacyHKCUVars[] = {
    "LUBAN_DATA", "LUBAN_CACHE", "LUBAN_STATE", "LUBAN_CONFIG",
};

// Quote helpers per shell. None of these are perfect (they assume the
// values don't contain the delimiter being used), but luban-managed paths
// don't have single/double quotes, so it works in practice.
std::string quote_bash(const std::string& s) {
    // Bash single-quoted: only ' itself needs special handling.
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\"'\"'"; else out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

std::string quote_cmd(const std::string& s) {
    // CMD: wrap whole "set" in double quotes so spaces / & / ; don't break.
    // Per `set "NAME=value"` syntax, the value is everything between = and ".
    std::string out;
    for (char c : s) {
        if (c == '%') out += "%%"; else out.push_back(c);
    }
    return out;
}

std::string quote_pwsh(const std::string& s) {
    // PowerShell single-quoted: '' escapes a literal '. No backtick parsing.
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "''"; else out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

// Emit eval-able env exports + PATH prepend for the requested shell. Reads
// env_snapshot::path_dirs() (toolchain bin dirs + bin_dir) and env_dict()
// (VCPKG_ROOT / vcpkg caches / EM_CONFIG) so the output mirrors exactly
// what luban-spawned children see.
//
// Use case: CI scripts, one-off containers, fresh shells where running
// `luban env --user` (which writes HKCU) is undesirable. Like
// `eval "$(luban env --print --shell bash)"`.
void print_shell_exports(std::string_view shell) {
    auto dirs = env_snapshot::path_dirs();
    auto extras = env_snapshot::env_dict();

    if (shell == "bash") {
        // POSIX-style. PATH separator is ':'; we convert backslashes to forward
        // slashes since this output is meant for git-bash / WSL / msys2 / Linux.
        std::string joined;
        for (size_t i = 0; i < dirs.size(); ++i) {
            if (i) joined.push_back(':');
            std::string s = dirs[i].string();
            for (auto& c : s) if (c == '\\') c = '/';
            joined += s;
        }
        std::cout << "export PATH=" << quote_bash(joined + ":${PATH:-}") << "\n";
        for (auto& [k, v] : extras) {
            std::cout << "export " << k << "=" << quote_bash(v) << "\n";
        }
    } else if (shell == "cmd") {
        // CMD batch. PATH separator is ';'. `set "name=value"` quotes the whole
        // assignment so embedded spaces / & don't break.
        std::string joined;
        for (size_t i = 0; i < dirs.size(); ++i) {
            if (i) joined.push_back(';');
            joined += dirs[i].string();
        }
        std::cout << "set \"PATH=" << quote_cmd(joined) << ";%PATH%\"\r\n";
        for (auto& [k, v] : extras) {
            std::cout << "set \"" << k << "=" << quote_cmd(v) << "\"\r\n";
        }
    } else if (shell == "powershell" || shell == "pwsh") {
        // PowerShell. Path separator follows OS; on Windows that's ';'.
        std::string joined;
        for (size_t i = 0; i < dirs.size(); ++i) {
            if (i) joined.push_back(';');
            joined += dirs[i].string();
        }
        std::cout << "$env:PATH = " << quote_pwsh(joined) << " + ';' + $env:PATH\r\n";
        for (auto& [k, v] : extras) {
            std::cout << "$env:" << k << " = " << quote_pwsh(v) << "\r\n";
        }
    } else {
        log::errf("--shell {} unrecognized; supported: bash, cmd, powershell", shell);
    }
}

int run_env(const cli::ParsedArgs& a) {
    fs::path bin = paths::bin_dir();

    auto get_flag = [&](const char* name) {
        auto it = a.flags.find(name);
        return it != a.flags.end() && it->second;
    };
    auto get_opt = [&](const char* name) -> std::string {
        auto it = a.opts.find(name);
        return (it != a.opts.end()) ? it->second : std::string();
    };

    // --print suppresses status output and emits eval-able exports only.
    if (get_flag("print")) {
        std::string shell = get_opt("shell");
        if (shell.empty()) shell = "bash";  // default: most CI runs bash
        print_shell_exports(shell);
        return 0;
    }

    log::infof("luban data dir : {}", paths::data_dir().string());
    log::infof("PATH directory : {}", bin.string());

    // --user: rustup-style. Add bin_dir() to HKCU PATH and set the env vars
    // tools outside luban need to see (VCPKG_ROOT for cmake presets, EM_CONFIG
    // for emcc). After this, any new shell sees luban's toolchain through PATH
    // and these tools resolve their config without further activation.
    if (get_flag("user")) {
        if (win_path::add_to_user_path(bin)) {
            log::okf("added {} to HKCU PATH", bin.string());
        } else {
            log::infof("{} already on HKCU PATH (no change)", bin.string());
        }

        auto recs = registry::load_installed();

        // VCPKG_ROOT: CMakePresets.json uses
        //   $env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake
        // so the var must be in HKCU for users running cmake outside luban.
        auto vcpkg_it = recs.find("vcpkg");
        if (vcpkg_it != recs.end() && !vcpkg_it->second.toolchain_dir.empty()) {
            fs::path vcpkg_root = paths::toolchain_dir(vcpkg_it->second.toolchain_dir);
            if (win_path::set_user_env("VCPKG_ROOT", vcpkg_root.string())) {
                log::okf("set HKCU VCPKG_ROOT = {}", vcpkg_root.string());
            }
        } else {
            log::info("VCPKG_ROOT not set (no vcpkg in registry; run `luban setup --with vcpkg` first)");
        }

        // EM_CONFIG: emscripten's config file lives at <config>/emscripten/config
        // (XDG-respecting, written by component.cpp). Setting EM_CONFIG lets
        // users run emcc directly in any shell — no activation, no PATH dance.
        if (recs.find("emscripten") != recs.end()) {
            fs::path em_config = paths::config_dir() / "emscripten" / "config";
            if (win_path::set_user_env("EM_CONFIG", em_config.string())) {
                log::okf("set HKCU EM_CONFIG = {}", em_config.string());
            }
        }

        log::info("open a new shell for the changes to take effect.");
    }

    // --unset-user: reverse --user. bin_dir is luban-owned (<data>/bin),
    // so safe to remove from HKCU PATH; users who want luban-managed
    // toolchains in their shells should run --user again.
    if (get_flag("unset-user")) {
        if (win_path::remove_from_user_path(bin)) {
            log::okf("removed {} from HKCU PATH", bin.string());
        } else {
            log::infof("{} not on HKCU PATH (nothing to do)", bin.string());
        }
        for (const char* name : kLegacyHKCUVars) {
            win_path::unset_user_env(name);
        }
        win_path::unset_user_env("VCPKG_ROOT");
        win_path::unset_user_env("EM_CONFIG");
        log::ok("removed legacy LUBAN_* and VCPKG_ROOT / EM_CONFIG from HKCU env");
    }

    return 0;
}

}  // namespace

void register_env() {
    cli::Subcommand c;
    c.name = "env";
    c.help = "show env state; register bin_dir on HKCU PATH (rustup-style)";
    c.group = "setup";
    c.long_help =
        "  Manage environment integration. With no flags, prints the bin dir\n"
        "  luban writes toolchain shims into (<data>/bin, luban-owned).\n"
        "\n"
        "  --user                Add bin_dir() to HKCU PATH + set HKCU\n"
        "                        VCPKG_ROOT (and EM_CONFIG if emscripten).\n"
        "                        After this, any new shell can run cmake /\n"
        "                        clang / ninja / etc. through luban-managed\n"
        "                        shims. The dir is luban-only (cargo's\n"
        "                        ~/.cargo/bin/ pattern); luban.exe itself\n"
        "                        lives under ~/.local/bin/ (XDG).\n"
        "  --unset-user          Reverse --user. Also cleans up legacy\n"
        "                        LUBAN_* HKCU env vars from pre-v0.2 installs.\n"
        "  --print [--shell X]   Emit eval-able env exports for shell X\n"
        "                        (bash | cmd | powershell). Default: bash.\n"
        "                        For CI / containers / fresh shells where\n"
        "                        writing HKCU is undesirable. Example:\n"
        "                          eval \"$(luban env --print --shell bash)\"";
    c.flags = {"user", "unset-user", "print"};
    c.opts = {{"shell", ""}};
    c.examples = {
        "luban env\tShow current env state",
        "luban env --user\tOne-time HKCU PATH registration (rustup-style)",
        "luban env --unset-user\tUndo the above + clean legacy vars",
        "luban env --print --shell bash\tEmit `export ...` lines for `eval`",
    };
    c.run = run_env;
    cli::register_subcommand(std::move(c));
}

}  // namespace luban::commands
