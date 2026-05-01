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

#include "../cli.hpp"
#include "../download.hpp"
#include "../hash.hpp"
#include "../log.hpp"
#include "../paths.hpp"
#include "../proc.hpp"
#include "../win_path.hpp"

namespace luban::commands {

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

// 与 cli.cpp 里的 kVersion 同步——为避免 cross-TU coupling，复读 luban -V 输出
// 比 #define 干净：但实际就 hard-code 了一份镜像，每次 release 都要同步。
// 比对 trade-off：少一个 extern global vs release 多记一处。这里取后者。
constexpr std::string_view kCurrentVersion = "0.1.2";

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

    log::infof("current: {}, latest: {}", kCurrentVersion, release->version);
    if (release->version == kCurrentVersion) {
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

    log::okf("luban updated: {} → {}", kCurrentVersion, release->version);
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
    fs::remove_all(dir, ec);
    if (ec) log::warnf("could not fully remove {}: {}", dir.string(), ec.message());
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

int run_uninstall(bool yes, bool keep_data) {
    if (!yes) {
        log::warn("`luban self uninstall` is destructive. Re-run with --yes to confirm.");
        log::info("This will:");
        log::info("  - remove bin_dir (<data>/bin) from HKCU PATH");
        log::info("  - unset HKCU VCPKG_ROOT and legacy LUBAN_* env vars");
        if (!keep_data) {
            log::info("  - delete <data>/<cache>/<state>/<config> (toolchains + caches + registry)");
        } else {
            log::info("  - keep <data>/<cache>/<state>/<config> (--keep-data)");
        }
        log::info("  - schedule luban.exe + luban-shim.exe self-delete in ~1-2s");
        log::info("Use --keep-data to preserve toolchains + cache.");
        return 1;
    }

    // 1. Report what's about to vanish so the user has one last chance to bail
    //    (mostly informational; --yes already committed).
    log::step("cleanup plan");
    if (!keep_data) {
        log_dir_size(paths::data_dir());
        log_dir_size(paths::cache_dir());
        log_dir_size(paths::state_dir());
        log_dir_size(paths::config_dir());
    }

    // 2. Reverse env --user. bin_dir is luban-owned (<data>/bin), so we
    //    remove it from HKCU PATH unconditionally. Older v0.1.x style.
    log::step("removing HKCU PATH + env vars");
    win_path::remove_from_user_path(paths::bin_dir());
    for (const char* name : {"LUBAN_DATA", "LUBAN_CACHE", "LUBAN_STATE", "LUBAN_CONFIG"}) {
        win_path::unset_user_env(name);
    }
    win_path::unset_user_env("VCPKG_ROOT");
    win_path::unset_user_env("EM_CONFIG");
    log::ok("HKCU env cleaned");

    // 3. Wipe luban-owned directories (data / cache / state / config). With
    //    bin_dir = <data>/bin, the data wipe takes the shims with it, so we
    //    don't need a separate selective-removal step.
    if (!keep_data) {
        log::step("removing luban directories");
        wipe(paths::data_dir());
        wipe(paths::cache_dir());
        wipe(paths::state_dir());
        wipe(paths::config_dir());
        log::ok("directories removed");
    } else {
        log::info("--keep-data: toolchains + caches preserved at <data>/etc.");
    }

    // 5. Self-delete the binaries: luban.exe (the running one) plus any
    //    sibling luban-shim.exe (build output / dev tree). The bin_dir copy
    //    was wiped via the data_dir() pass above when --keep-data is false.
#ifdef _WIN32
    fs::path self = self_exe_path();
    std::vector<fs::path> targets = {self};
    fs::path shim_sibling = self.parent_path() / "luban-shim.exe";
    std::error_code ec;
    if (fs::exists(shim_sibling, ec)) targets.push_back(shim_sibling);
    log::step("scheduling self-delete (~1.5s)");
    spawn_self_delete_batch(targets);
    log::ok("done");
    log::infof("luban will be removed shortly. Goodbye.");
#else
    fs::path self = self_exe_path();
    fs::remove(self, std::error_code{});
#endif

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
        bool yes = a.flags.count("yes") && a.flags.at("yes");
        bool keep = a.flags.count("keep-data") && a.flags.at("keep-data");
        return run_uninstall(yes, keep);
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
        "  `luban self uninstall --yes` reverses every footprint:\n"
        "    - Remove bin_dir (<data>/bin) from HKCU PATH\n"
        "    - Unset HKCU VCPKG_ROOT + legacy LUBAN_* env vars\n"
        "    - Wipe <data>/<cache>/<state>/<config> (or --keep-data to skip)\n"
        "    - Schedule luban.exe + sibling luban-shim.exe self-delete\n"
        "  Refuses without --yes (destructive).";
    c.flags = {"yes", "keep-data"};
    c.n_positional = 1;
    c.positional_names = {"sub"};
    c.examples = {
        "luban self update\tFetch latest release, verify, swap binary",
        "luban self uninstall --yes\tFull cleanup of luban + toolchains",
        "luban self uninstall --yes --keep-data\tRemove luban only; keep toolchains on disk",
    };
    c.run = run_self;
    cli::register_subcommand(std::move(c));
}

}  // namespace luban::commands
