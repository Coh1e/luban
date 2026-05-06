// `luban self update`     — pull latest release, verify SHA256, swap binary
// `luban self uninstall`  — reverse all of luban's footprint on this machine

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#include "json.hpp"

#include "luban/version.hpp"

#include "../cli.hpp"
#include "../download.hpp"
#include "../generation.hpp"
#include "../hash.hpp"
#include "../log.hpp"
#include "../msvc_env.hpp"
#include "../paths.hpp"
#include "../proc.hpp"
#include "../win_path.hpp"

namespace luban::commands {

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

// Pulled from the cmake-generated header — single source of truth in
// CMakeLists.txt's project(VERSION ...). Was a hand-synced string literal
// pre-v0.2.
using luban::kLubanVersion;

constexpr const char* kReleaseApi = "https://api.github.com/repos/Coh1e/luban/releases/latest";

// ---- self path ----
fs::path self_exe_path() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4];
    DWORD got = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (got == 0) return fs::current_path() / "luban.exe";
    return fs::path(std::wstring(buf, got));
#else
    return fs::current_path() / "luban";
#endif
}

// ---- update ----

// {"tag_name": "v0.1.1", "assets": [{"name": "luban.exe", "browser_download_url": "..."}, ...]}
struct LatestRelease {
    std::string tag;            // "v0.1.1"
    std::string version;        // "0.1.1" (tag minus leading 'v')
    std::string luban_url;
    std::string sha_url;
};

std::optional<LatestRelease> fetch_latest_release() {
    auto rc = download::fetch_text(kReleaseApi, 15);
    if (!rc) {
        log::errf("could not fetch release info: {}", rc.error().message);
        return std::nullopt;
    }
    json doc;
    try { doc = json::parse(*rc); } catch (...) {
        log::err("could not parse release JSON");
        return std::nullopt;
    }
    LatestRelease r;
    r.tag = doc.value("tag_name", "");
    r.version = r.tag;
    if (!r.version.empty() && r.version.front() == 'v') r.version.erase(0, 1);
    if (!doc.contains("assets") || !doc["assets"].is_array()) return std::nullopt;
    for (auto& a : doc["assets"]) {
        std::string name = a.value("name", "");
        std::string url = a.value("browser_download_url", "");
        if (name == "luban.exe") r.luban_url = url;
        if (name == "SHA256SUMS") r.sha_url = url;
    }
    if (r.luban_url.empty()) {
        log::err("release has no luban.exe asset");
        return std::nullopt;
    }
    return r;
}

// Parse SHA256SUMS line for the file we care about. Format: "<hex>  <name>"
std::optional<std::string> parse_sums_for(const std::string& text, std::string_view name) {
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        // Find double-space (sha256sum default format)
        auto sp = line.find("  ");
        if (sp == std::string::npos) continue;
        std::string hex = line.substr(0, sp);
        std::string fname = line.substr(sp + 2);
        // Strip trailing whitespace
        while (!fname.empty() && (fname.back() == '\r' || fname.back() == '\n' || fname.back() == ' '))
            fname.pop_back();
        if (fname == name) return hex;
    }
    return std::nullopt;
}

int run_update() {
    auto release = fetch_latest_release();
    if (!release) return 1;

    log::infof("current: {}, latest: {}", kLubanVersion, release->version);
    if (release->version == kLubanVersion) {
        log::ok("already up to date");
        return 0;
    }

    fs::path self = self_exe_path();
    fs::path tmp = self;
    tmp += ".new";

    log::stepf("downloading {}", release->luban_url);
    download::DownloadOptions opts;
    opts.label = "luban.exe (v" + release->version + ")";
    opts.retries = 3;
    opts.timeout_seconds = 60;
    auto dl = download::download(release->luban_url, tmp, opts);
    if (!dl) {
        log::errf("download failed: {}", dl.error().message);
        return 1;
    }

    if (!release->sha_url.empty()) {
        log::step("verifying SHA256");
        auto sums_text = download::fetch_text(release->sha_url, 15);
        if (sums_text) {
            auto expected = parse_sums_for(*sums_text, "luban.exe");
            if (expected) {
                hash::HashSpec exp{hash::Algorithm::Sha256, *expected};
                if (!hash::verify_file(tmp, exp)) {
                    std::error_code ec;
                    fs::remove(tmp, ec);
                    log::errf("SHA256 mismatch — refusing to swap");
                    return 1;
                }
                log::ok("SHA256 verified");
            } else {
                log::warn("SHA256SUMS does not list luban.exe; download SHA written but unverified");
            }
        } else {
            log::warn("could not fetch SHA256SUMS; using stream-computed SHA only");
        }
    }

#ifdef _WIN32
    // Windows: running exe is locked. Move current → .old, new → current.
    // .old will be deletable next reboot or by user later.
    fs::path backup = self; backup += ".old";
    std::error_code ec;
    fs::remove(backup, ec);  // any stale .old
    if (!MoveFileExW(self.wstring().c_str(), backup.wstring().c_str(),
                     MOVEFILE_REPLACE_EXISTING)) {
        log::errf("could not move running exe aside (err={})", GetLastError());
        fs::remove(tmp, ec);
        return 1;
    }
    if (!MoveFileExW(tmp.wstring().c_str(), self.wstring().c_str(),
                     MOVEFILE_REPLACE_EXISTING)) {
        log::errf("could not install new exe (err={})", GetLastError());
        // try to restore
        MoveFileExW(backup.wstring().c_str(), self.wstring().c_str(),
                    MOVEFILE_REPLACE_EXISTING);
        return 1;
    }
    // Schedule .old for deletion at reboot — non-fatal if it fails.
    MoveFileExW(backup.wstring().c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
#else
    std::error_code ec;
    fs::rename(tmp, self, ec);
    if (ec) return 1;
#endif

    log::okf("luban updated: {} → {}", kLubanVersion, release->version);
    log::infof("re-run any luban command to use the new version");
    return 0;
}

// ---- uninstall ----

void log_dir_size(const fs::path& dir) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) return;
    std::uintmax_t total = 0;
    for (auto& e : fs::recursive_directory_iterator(dir, ec)) {
        if (e.is_regular_file(ec)) total += e.file_size(ec);
    }
    double mb = static_cast<double>(total) / (1024.0 * 1024.0);
    log::infof("  {} ({:.1f} MB)", dir.string(), mb);
}

void wipe(const fs::path& dir) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) return;  // idempotent: re-uninstall is silent
    fs::remove_all(dir, ec);
    if (ec) log::warnf("could not fully remove {}: {}", dir.string(), ec.message());
}

// Sweep luban-owned shim files out of the shared XDG bin home.
// The XDG bin home (~/.local/bin) is shared with uv / pipx / claude-code,
// so we MUST NOT remove the directory itself or strip it from PATH on
// uninstall. Instead, walk every generation's ToolRecord and remove the
// individual `.cmd` (and twin `.exe`) files we know we wrote. The shim-
// table at <data>/bin/.shim-table.json (legacy `luban shim` writer) is
// also walked for the v0.x → v1.0 migration window.
//
// Idempotent: missing files / missing state dir / missing shim-table all
// silently no-op. Returns count of files removed for logging.
int sweep_owned_shims() {
    int removed = 0;
    std::error_code ec;

    // 1. Generation snapshots — authoritative for v1.0+ blueprint shims.
    for (int id : luban::generation::list_ids()) {
        auto g = luban::generation::read(id);
        if (!g) continue;
        auto try_remove = [&](const std::string& shim_str) {
            if (shim_str.empty()) return;
            fs::path p(shim_str);
            if (fs::remove(p, ec)) ++removed;
            // .cmd was the recorded path; an .exe twin may live alongside.
            fs::path twin = p; twin.replace_extension(".exe");
            if (twin != p && fs::remove(twin, ec)) ++removed;
        };
        for (auto& [_name, rec] : g->tools) {
            try_remove(rec.shim_path);
            for (auto& s : rec.shim_paths_secondary) try_remove(s);
        }
    }

    // 2. Legacy `luban shim` table at <data>/bin/.shim-table.json.
    fs::path table = paths::bin_dir() / ".shim-table.json";
    if (fs::exists(table, ec)) {
        try {
            std::ifstream in(table, std::ios::binary);
            json j;
            in >> j;
            if (j.is_object()) {
                for (auto it = j.begin(); it != j.end(); ++it) {
                    std::string alias = it.key();
                    for (const char* ext : {".cmd", ".exe"}) {
                        fs::path p = paths::bin_dir() / (alias + ext);
                        if (fs::remove(p, ec)) ++removed;
                        fs::path q = paths::xdg_bin_home() / (alias + ext);
                        if (fs::remove(q, ec)) ++removed;
                    }
                }
            }
        } catch (...) {
            // Malformed table: skip silently. The directory wipe below
            // will still get whatever's left.
        }
    }
    return removed;
}

#ifdef _WIN32
// Spawn a batch script that waits for our exit, deletes our exe(s), then deletes itself.
// Returns immediately (does not wait). Caller should exit shortly after.
void spawn_self_delete_batch(const std::vector<fs::path>& targets) {
    fs::path tmp = fs::temp_directory_path() / "luban-uninstall.bat";
    {
        std::ofstream of(tmp, std::ios::binary | std::ios::trunc);
        of << "@echo off\r\n";
        // 1.5 sec ping-based delay so luban.exe has time to fully exit + release lock
        of << "ping 127.0.0.1 -n 2 >nul\r\n";
        for (auto& t : targets) {
            of << "del /F /Q \"" << t.string() << "\" 2>nul\r\n";
        }
        of << "del /F /Q \"%~f0\"\r\n";  // self-delete batch
    }
    // Spawn detached cmd.exe to run it; STARTUPINFO no window
    std::wstring wcmdline = L"cmd.exe /c \"" + tmp.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mut(wcmdline.begin(), wcmdline.end());
    mut.push_back(L'\0');
    if (CreateProcessW(nullptr, mut.data(), nullptr, nullptr, FALSE,
                       DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
                       nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}
#endif

// True iff `p` is lexically inside one of luban's four canonical homes.
// Used by safe_unset_user_env / safe_remove_path_entry to gate destructive
// HKCU edits on ownership: if the user pointed VCPKG_ROOT (or any
// msvc-captured env var) at a directory outside luban's data/cache/state/
// config trees, luban must NOT clear it on uninstall.
bool is_inside_luban_dirs(const fs::path& p) {
    if (p.empty()) return false;
    fs::path probe;
    try {
        probe = p.lexically_normal();
    } catch (...) {
        return false;
    }
    auto under = [&](const fs::path& root) -> bool {
        if (root.empty()) return false;
        fs::path normalized_root = root.lexically_normal();
        auto rel = probe.lexically_relative(normalized_root);
        std::string rs = rel.string();
        return !rs.empty() && rs != "." && rs.rfind("..", 0) != 0;
    };
    return under(paths::data_dir()) || under(paths::cache_dir()) ||
           under(paths::state_dir()) || under(paths::config_dir());
}

// Unset HKCU\Environment\<name> only if its current value points inside
// luban's own dirs. Names with no recorded value are unset unconditionally
// (no possible side effect on user data). Returns true if we actually
// unset something, false if we preserved (with a log line either way).
bool safe_unset_user_env(const std::string& name) {
    auto cur = win_path::get_user_env(name);
    if (!cur) {
        // Not present in HKCU — nothing to do. Silent (the common case
        // for LUBAN_* on most installs).
        return false;
    }
    if (cur->empty()) {
        // Empty value — degenerate case, just clear.
        win_path::unset_user_env(name);
        return true;
    }
    // Heuristic: treat the whole value as a path. PATH-list-style
    // multi-segment values (e.g. someone hand-crafted VCPKG_ROOT="a;b")
    // are rejected by `is_inside_luban_dirs` and preserved — that's
    // fine; the user clearly hand-rolled it and we don't own it.
    if (!is_inside_luban_dirs(fs::path(*cur))) {
        log::infof("preserving HKCU {}={} (points outside luban dirs)",
                   name, *cur);
        return false;
    }
    win_path::unset_user_env(name);
    log::infof("unset HKCU {}", name);
    return true;
}

// Same ownership gate for HKCU PATH segment removal. Used for msvc-captured
// PATH entries — the user might have re-pointed them at a system MSVC
// install after luban captured a different one.
bool safe_remove_user_path(const fs::path& dir) {
    if (dir.empty()) return false;
    if (!is_inside_luban_dirs(dir)) {
        log::infof("preserving HKCU PATH entry {} (outside luban dirs)",
                   dir.string());
        return false;
    }
    return win_path::remove_from_user_path(dir);
}

struct UninstallOptions {
    bool yes = false;
    bool keep_data = false;         // skip ALL dir wipes + skip self-delete
    bool keep_toolchains = false;   // skip <data>/{toolchains,bin} + <cache> + skip self-delete
};

int run_uninstall(const UninstallOptions& opts) {
    if (!opts.yes) {
        log::warn("`luban self uninstall` is destructive. Re-run with --yes to confirm.");
        log::info("This will:");
        log::info("  - remove luban-owned entries from HKCU PATH");
        log::info("  - unset HKCU env vars whose value points inside luban dirs");
        log::info("    (VCPKG_ROOT / EM_CONFIG / msvc captures pointing elsewhere are preserved)");
        if (opts.keep_data) {
            log::info("  - keep <data>/<cache>/<state>/<config> (--keep-data)");
        } else if (opts.keep_toolchains) {
            log::info("  - wipe <data>/{store,bp_sources,registry,env} + <state> + <config>");
            log::info("  - keep <data>/{toolchains,bin} + <cache> (--keep-toolchains)");
        } else {
            log::info("  - delete <data>/<cache>/<state>/<config> (toolchains + caches + registry)");
        }
        if (opts.keep_data || opts.keep_toolchains) {
            log::info("  - keep luban.exe + luban-shim.exe in place");
        } else {
            log::info("  - schedule luban.exe + luban-shim.exe self-delete in ~1-2s");
        }
        log::info("Flags: --keep-data (full preserve) | --keep-toolchains (clean bp state only)");
        return 1;
    }

    // 1. Report what's about to vanish so the user has one last chance to bail
    //    (mostly informational; --yes already committed).
    log::step("cleanup plan");
    if (!opts.keep_data) {
        if (opts.keep_toolchains) {
            // Only the dirs we're about to actually remove.
            for (const char* sub : {"store", "bp_sources", "registry", "env"}) {
                log_dir_size(paths::data_dir() / sub);
            }
            log_dir_size(paths::state_dir());
            log_dir_size(paths::config_dir());
        } else {
            log_dir_size(paths::data_dir());
            log_dir_size(paths::cache_dir());
            log_dir_size(paths::state_dir());
            log_dir_size(paths::config_dir());
        }
    }

    // 2. HKCU env vars. Read msvc captures BEFORE we wipe state_dir
    //    (msvc-env.json lives there).
    log::step("HKCU PATH + env vars");
    auto msvc_cap = msvc_env::load();

    // bin_dir is luban-owned by construction. With keep_toolchains we
    // intentionally leave it on PATH so the user's shimmed tools keep
    // working post-cleanup.
    if (!opts.keep_toolchains) {
        win_path::remove_from_user_path(paths::bin_dir());
    }
    // LUBAN_* always belong to luban — ownership check can't fail. Use
    // the same safe wrapper anyway for consistent logging.
    for (const char* name : {"LUBAN_DATA", "LUBAN_CACHE", "LUBAN_STATE", "LUBAN_CONFIG"}) {
        safe_unset_user_env(name);
    }
    safe_unset_user_env("VCPKG_ROOT");
    safe_unset_user_env("EM_CONFIG");

    if (msvc_cap) {
        for (auto& [k, _] : msvc_cap->vars) safe_unset_user_env(k);
        std::string s = msvc_cap->path_addition;
        size_t start = 0;
        while (start <= s.size()) {
            size_t end = s.find(';', start);
            if (end == std::string::npos) end = s.size();
            std::string dir = s.substr(start, end - start);
            if (!dir.empty()) safe_remove_user_path(fs::path(dir));
            if (end == s.size()) break;
            start = end + 1;
        }
    }
    log::ok("HKCU env cleaned");

    // 3. Sweep luban-owned shims out of the shared XDG bin home. Must run
    //    BEFORE state_dir wipe because we read generation snapshots from
    //    <state>/generations/ to know which files to delete. We never
    //    remove ~/.local/bin/ itself (shared with uv/pipx/claude-code).
    if (!opts.keep_data) {
        log::step("sweeping luban-owned shims from " +
                  paths::xdg_bin_home().string());
        int removed = sweep_owned_shims();
        log::okf("removed {} shim file(s)", removed);
    }

    // 4. Directory wipe. Three modes:
    //   - keep_data:        no-op
    //   - keep_toolchains:  selective — only luban-private subdirs of data
    //                       + state + config; cache and toolchains/bin survive
    //   - default:          all four dirs gone
    if (opts.keep_data) {
        log::info("--keep-data: all luban dirs preserved on disk");
    } else if (opts.keep_toolchains) {
        log::step("selective wipe (--keep-toolchains)");
        for (const char* sub : {"store", "bp_sources", "registry", "env"}) {
            wipe(paths::data_dir() / sub);
        }
        wipe(paths::state_dir());
        wipe(paths::config_dir());
        log::ok("bp state cleared; toolchains + cache preserved");
    } else {
        log::step("removing luban directories");
        wipe(paths::data_dir());
        wipe(paths::cache_dir());
        wipe(paths::state_dir());
        wipe(paths::config_dir());
        log::ok("directories removed");
    }

    // 4. Self-delete only on the full uninstall path. Both keep_data and
    //    keep_toolchains imply "user wants to keep using luban afterwards"
    //    — self-delete would defeat that.
    if (!opts.keep_data && !opts.keep_toolchains) {
#ifdef _WIN32
        fs::path self = self_exe_path();
        std::vector<fs::path> targets = {self};
        fs::path shim_sibling = self.parent_path() / "luban-shim.exe";
        std::error_code ec;
        if (fs::exists(shim_sibling, ec)) targets.push_back(shim_sibling);
        // Also sweep .old leftovers from prior self-updates.
        for (const char* sib : {"luban.exe.old", "luban-shim.exe.old"}) {
            fs::path p = self.parent_path() / sib;
            if (fs::exists(p, ec)) targets.push_back(p);
        }
        log::step("scheduling self-delete (~1.5s)");
        spawn_self_delete_batch(targets);
        log::ok("done");
        log::infof("luban will be removed shortly. Goodbye.");
#else
        fs::path self = self_exe_path();
        std::error_code ec;
        fs::remove(self, ec);  // best-effort; failure is non-fatal
#endif
    } else {
        log::infof("luban.exe preserved; re-run anytime");
    }

    return 0;
}

// ---- dispatch ----

int run_self(const cli::ParsedArgs& a) {
    if (a.positional.empty()) {
        log::err("usage: luban self <update|uninstall>");
        return 2;
    }
    const std::string& sub = a.positional[0];
    if (sub == "update") return run_update();
    if (sub == "uninstall") {
        UninstallOptions opts;
        opts.yes = a.flags.count("yes") && a.flags.at("yes");
        opts.keep_data = a.flags.count("keep-data") && a.flags.at("keep-data");
        opts.keep_toolchains = a.flags.count("keep-toolchains") &&
                               a.flags.at("keep-toolchains");
        if (opts.keep_data && opts.keep_toolchains) {
            log::err("--keep-data and --keep-toolchains are mutually exclusive");
            return 2;
        }
        return run_uninstall(opts);
    }
    log::errf("unknown self subcommand: {} (expected update | uninstall)", sub);
    return 2;
}

}  // namespace

void register_self() {
    cli::Subcommand c;
    c.name = "self";
    c.help = "manage the luban binary itself (update | uninstall)";
    c.group = "advanced";
    c.long_help =
        "  `luban self update` fetches the latest release from\n"
        "  github.com/Coh1e/luban, verifies SHA256, and atomically swaps the\n"
        "  running luban.exe. The previous binary is moved to luban.exe.old\n"
        "  and scheduled for delete-on-reboot.\n"
        "\n"
        "  `luban self uninstall --yes` reverses luban's footprint:\n"
        "    - Remove bin_dir (<data>/bin) from HKCU PATH\n"
        "    - Unset HKCU env vars whose value points inside luban dirs\n"
        "      (VCPKG_ROOT / EM_CONFIG / msvc captures pointing elsewhere\n"
        "       are PRESERVED — luban won't clobber a hand-rolled env)\n"
        "    - Wipe <data>/<cache>/<state>/<config>\n"
        "    - Schedule luban.exe + sibling luban-shim.exe self-delete\n"
        "  Refuses without --yes (destructive).\n"
        "\n"
        "  Flags (mutually exclusive):\n"
        "    --keep-data         skip ALL dir wipes; only env/PATH cleanup\n"
        "                        (binary preserved; lighter than full uninstall)\n"
        "    --keep-toolchains   wipe <data>/{store,bp_sources,registry,env}\n"
        "                        + <state> + <config>; preserve\n"
        "                        <data>/{toolchains,bin} + <cache> + binary\n"
        "                        (use to reset bp state without losing dev env)";
    c.flags = {"yes", "keep-data", "keep-toolchains"};
    c.n_positional = 1;
    c.positional_names = {"sub"};
    c.examples = {
        "luban self update\tFetch latest release, verify, swap binary",
        "luban self uninstall --yes\tFull cleanup of luban + toolchains",
        "luban self uninstall --yes --keep-data\tEnv/PATH cleanup only; preserve disk",
        "luban self uninstall --yes --keep-toolchains\tReset bp state; keep cmake/vcpkg",
    };
    c.run = run_self;
    cli::register_subcommand(std::move(c));
}

}  // namespace luban::commands
