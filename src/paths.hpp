#pragma once
// XDG-first directory resolution.
// 1:1 port of luban_boot/paths.py:28-90 — must match Python output bit-for-bit
// because installed.json is produced by Python today.

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace luban::paths {

namespace fs = std::filesystem;

constexpr std::string_view APP_NAME = "luban";

// Env helpers — strip whitespace, treat empty as missing.
std::optional<fs::path> from_env(std::string_view var);
fs::path home();

// 4 canonical homes (XDG-first, with Linux/macOS/Windows fallbacks).
fs::path data_dir();
fs::path cache_dir();
fs::path state_dir();
fs::path config_dir();

// Sub-locations (all derived).
fs::path store_dir();
fs::path store_sha256_dir();
fs::path toolchains_dir();
fs::path toolchain_dir(std::string_view name);
fs::path bin_dir();
fs::path env_dir();
fs::path registry_dir();
fs::path buckets_dir();
fs::path overlay_dir();
fs::path downloads_dir();
fs::path vcpkg_binary_cache_dir();
fs::path installed_json_path();
fs::path last_sync_path();
fs::path logs_dir();
fs::path selection_json_path();
fs::path config_toml_path();

// Doctor / setup walks: ordered name → path.
std::vector<std::pair<std::string, fs::path>> all_dirs();

void ensure_dirs();
bool same_volume(const fs::path& a, const fs::path& b);

}  // namespace luban::paths
