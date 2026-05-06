#include "paths.hpp"

#include <algorithm>
#include <cstdlib>
#include <vector>

#ifdef _WIN32
#include <shlobj.h>
#include "util/win.hpp"
#endif

namespace luban::paths {

namespace {

#if defined(_WIN32)
constexpr bool kIsWindows = true;
constexpr bool kIsMacOS = false;
#elif defined(__APPLE__)
constexpr bool kIsWindows = false;
constexpr bool kIsMacOS = true;
#else
constexpr bool kIsWindows = false;
constexpr bool kIsMacOS = false;
#endif

std::string trim(std::string s) {
    auto not_space = [](unsigned char c) { return c != ' ' && c != '\t' && c != '\r' && c != '\n'; };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

std::optional<std::string> raw_env(std::string_view var) {
#ifdef _WIN32
    // GetEnvironmentVariableW: handles unicode env values, no MSVC _dupenv_s warning.
    std::wstring wname = win::from_utf8(var);
    DWORD n = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (n == 0) return std::nullopt;
    std::wstring buf(n, L'\0');
    DWORD got = GetEnvironmentVariableW(wname.c_str(), buf.data(), n);
    if (got == 0) return std::nullopt;
    buf.resize(got);
    return win::to_utf8(buf);
#else
    std::string name(var);
    const char* v = std::getenv(name.c_str());
    if (!v) return std::nullopt;
    return std::string(v);
#endif
}

// Expand ~ at the start. Mirrors Path.expanduser() — only literal "~" or "~/...".
fs::path expanduser(const std::string& s) {
    if (s.empty() || s[0] != '~') return fs::path(s);
    if (s.size() == 1) return home();
    if (s[1] == '/' || s[1] == '\\') return home() / fs::path(s.substr(2));
    return fs::path(s);
}

std::optional<fs::path> luban_prefix(std::string_view role) {
    auto p = from_env("LUBAN_PREFIX");
    if (!p) return std::nullopt;
    return *p / std::string(role);
}

#ifdef _WIN32
fs::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    if (SHGetKnownFolderPath(id, 0, nullptr, &raw) == S_OK && raw) {
        std::wstring w(raw);
        CoTaskMemFree(raw);
        return fs::path(win::to_utf8(w));
    }
    if (raw) CoTaskMemFree(raw);
    return home() / "AppData" / "Local";  // last-ditch fallback
}
#endif

fs::path localappdata() {
    auto v = from_env("LOCALAPPDATA");
    if (v) return *v;
#ifdef _WIN32
    return known_folder(FOLDERID_LocalAppData);
#else
    return home() / "AppData" / "Local";
#endif
}

fs::path resolve(std::string_view role,
                 std::string_view xdg_var,
                 const fs::path& linux_default,
                 const fs::path& macos_default,
                 const fs::path& windows_fallback) {
    if (auto p = luban_prefix(role)) return *p;
    if (auto p = from_env(xdg_var)) return *p / std::string(APP_NAME);
    if constexpr (kIsMacOS) return macos_default;
    if constexpr (kIsWindows) return windows_fallback;
    return linux_default;
}

}  // namespace

std::optional<fs::path> from_env(std::string_view var) {
    auto raw = raw_env(var);
    if (!raw) return std::nullopt;
    std::string trimmed = trim(*raw);
    if (trimmed.empty()) return std::nullopt;
    return expanduser(trimmed);
}

fs::path home() {
#ifdef _WIN32
    auto userprofile = raw_env("USERPROFILE");
    if (userprofile && !userprofile->empty()) return fs::path(*userprofile);
    auto drive = raw_env("HOMEDRIVE");
    auto path = raw_env("HOMEPATH");
    if (drive && path) return fs::path(*drive + *path);
    return known_folder(FOLDERID_Profile);
#else
    auto h = raw_env("HOME");
    if (h && !h->empty()) return fs::path(*h);
    return fs::path("/");
#endif
}

fs::path data_dir() {
    return resolve("data", "XDG_DATA_HOME",
                   home() / ".local" / "share" / std::string(APP_NAME),
                   home() / "Library" / "Application Support" / std::string(APP_NAME),
                   localappdata() / std::string(APP_NAME));
}

fs::path cache_dir() {
    return resolve("cache", "XDG_CACHE_HOME",
                   home() / ".cache" / std::string(APP_NAME),
                   home() / "Library" / "Caches" / std::string(APP_NAME),
                   localappdata() / std::string(APP_NAME) / "Cache");
}

fs::path state_dir() {
    return resolve("state", "XDG_STATE_HOME",
                   home() / ".local" / "state" / std::string(APP_NAME),
                   home() / "Library" / "Application Support" / std::string(APP_NAME) / "State",
                   localappdata() / std::string(APP_NAME) / "State");
}

fs::path config_dir() {
    // XDG_CONFIG_HOME on Linux. macOS uses Preferences. Windows: keep config
    // local (was roaming before; roaming syncs across domain machines but
    // installed.json is local — mismatch causes 're-install on every machine'
    // surprises, and selection.json is no use without matching toolchains).
    return resolve("config", "XDG_CONFIG_HOME",
                   home() / ".config" / std::string(APP_NAME),
                   home() / "Library" / "Preferences" / std::string(APP_NAME),
                   localappdata() / std::string(APP_NAME) / "Config");
}

fs::path store_dir() { return data_dir() / "store"; }
fs::path store_sha256_dir() { return store_dir() / "sha256"; }
fs::path toolchains_dir() { return data_dir() / "toolchains"; }
fs::path toolchain_dir(std::string_view name) { return toolchains_dir() / std::string(name); }

// luban-managed shim dir (cargo's ~/.cargo/bin/ pattern). One toolchain
// install can produce hundreds of shims (LLVM-MinGW alone yields ~280 cross-
// compiler aliases), so dropping them into the shared XDG ~/.local/bin would
// clobber the volume of files there and stress Defender real-time scanning
// during cleanup. Keep them under <data> where the dir is 100% luban-owned.
//
// luban.exe itself goes to ~/.local/bin/ via install.ps1 — that's user-level
// XDG, single-binary, joins uv/pipx/claude-code. Toolchain shims live here.
fs::path bin_dir() { return data_dir() / "bin"; }

// XDG bin home — ~/.local/bin/. Co-located with uv / pipx / claude-code
// shim space. v1.0+ blueprint shims and luban.exe install destination.
// Note: install.ps1 already writes luban.exe here; this function makes
// the path explicit for callers (env, blueprint_apply, shim_cmd).
//
// We deliberately don't honor XDG_BIN_HOME — POSIX uv / pipx / npm-style
// tools all hardcode ~/.local/bin/ regardless of that env var because
// the spec is aspirational. Following that crowd keeps PATH consistent.
fs::path xdg_bin_home() { return home() / ".local" / "bin"; }
fs::path env_dir() { return data_dir() / "env"; }
fs::path registry_dir() { return data_dir() / "registry"; }
fs::path buckets_dir() { return registry_dir() / "buckets"; }
fs::path overlay_dir() { return registry_dir() / "overlay"; }
fs::path bp_sources_dir() { return data_dir() / "bp_sources"; }
fs::path bp_sources_dir(std::string_view name) { return bp_sources_dir() / std::string(name); }
fs::path bp_sources_registry_path() { return config_dir() / "sources.toml"; }
fs::path downloads_dir() { return cache_dir() / "downloads"; }
fs::path vcpkg_binary_cache_dir() { return cache_dir() / "vcpkg-binary"; }
// vcpkg's three caches that v0.2 routes via env vars (env_snapshot:env_dict).
// vcpkg insists the dirs exist; without ensure_dirs creating them, the very
// first `luban build` against a freshly-installed luban panics with
// "Value of environment variable X_VCPKG_REGISTRIES_CACHE is not a directory".
fs::path vcpkg_cache_dir() { return cache_dir() / "vcpkg"; }
fs::path vcpkg_downloads_dir() { return vcpkg_cache_dir() / "downloads"; }
fs::path vcpkg_archives_dir() { return vcpkg_cache_dir() / "archives"; }
fs::path vcpkg_registries_dir() { return vcpkg_cache_dir() / "registries"; }
fs::path installed_json_path() { return state_dir() / "installed.json"; }
fs::path last_sync_path() { return state_dir() / ".last_sync"; }
fs::path logs_dir() { return state_dir() / "logs"; }
fs::path selection_json_path() { return config_dir() / "selection.json"; }
fs::path config_toml_path() { return config_dir() / "config.toml"; }

std::vector<std::pair<std::string, fs::path>> all_dirs() {
    return {
        {"data", data_dir()},
        {"cache", cache_dir()},
        {"state", state_dir()},
        {"config", config_dir()},
        {"store", store_dir()},
        {"store/sha256", store_sha256_dir()},
        {"toolchains", toolchains_dir()},
        {"bin", bin_dir()},
        {"env", env_dir()},
        {"registry", registry_dir()},
        {"registry/buckets", buckets_dir()},
        {"registry/overlay", overlay_dir()},
        {"bp_sources", bp_sources_dir()},
        {"downloads", downloads_dir()},
        {"vcpkg-binary", vcpkg_binary_cache_dir()},
        {"vcpkg/downloads",   vcpkg_downloads_dir()},
        {"vcpkg/archives",    vcpkg_archives_dir()},
        {"vcpkg/registries",  vcpkg_registries_dir()},
        {"logs", logs_dir()},
    };
}

void ensure_dirs() {
    std::error_code ec;
    for (auto& [_, p] : all_dirs()) fs::create_directories(p, ec);
}

bool same_volume(const fs::path& a, const fs::path& b) {
#ifdef _WIN32
    // Plan §7: don't canonicalize — junction may resolve to a different drive
    // than the user-typed path. Compare root_name() of the literal paths.
    auto root = [](const fs::path& p) {
        auto s = p.root_name().string();
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    return root(a) == root(b);
#else
    std::error_code ec;
    auto sa = fs::status(a, ec);
    auto sb = fs::status(b, ec);
    if (ec) return false;
    return sa.permissions() == sa.permissions();  // POSIX dev compare omitted; M2 covers it.
#endif
}

}  // namespace luban::paths
