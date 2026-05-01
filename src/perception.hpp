#pragma once
// Host-capability detection.
//
// Sniffs OS / CPU / RAM / available SIMD / known dev tools / XDG env so
// luban (and consumers like AGENTS.md generation, IDE plugins, AI agents)
// can give project-specific advice instead of blanket recommendations.
//
// Surfaced via `luban describe --host` (machine-readable JSON) and as a
// short summary in `luban doctor`. Pure read-only — never mutates state.
//
// Design notes:
//   * Zero new dependencies — uses Win32 (GetSystemInfo, __cpuid,
//     GlobalMemoryStatusEx) on Windows; /proc and uname on POSIX (M4).
//   * Detection is best-effort. A field that can't be resolved becomes an
//     empty string / -1 / nullopt rather than a hard error — agents can
//     work with partial info.
//   * No network. No registry probing beyond what's already needed by
//     paths.cpp.
//   * GPU / NPU detection is out of scope for v1 (DirectML / WinML pull
//     in COM); see open-questions.md.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "json.hpp"

namespace luban::perception {

// Single host snapshot. JSON-serializable via to_json. Fields kept flat
// (no nested objects beyond `xdg`) to make jq-style queries easy.
struct Host {
    // OS identity
    std::string os_name;        // "Windows", "Linux", "macOS"
    std::string os_version;     // "10.0.26200" / "5.15.0-..." / "14.4.1"
    std::string arch;           // "x86_64", "arm64"

    // CPU
    std::string cpu_vendor;     // "GenuineIntel", "AuthenticAMD", ""
    std::string cpu_brand;      // "Intel(R) Core(TM) i9-..." (when readable)
    int         cpu_cores = 0;          // physical, when distinguishable
    int         cpu_threads = 0;        // logical (online)
    std::vector<std::string> cpu_features;  // {"sse4_2","avx","avx2","avx512f"}

    // Memory (bytes, -1 if unknown)
    int64_t ram_total = -1;
    int64_t ram_available = -1;

    // Tools on PATH (just the binary names that resolved). Useful for
    // AGENTS.md to know what's already there before recommending installs.
    std::vector<std::string> tools_on_path;

    // XDG / luban env. Empty if not set.
    struct Xdg {
        std::string data_home;
        std::string cache_home;
        std::string state_home;
        std::string config_home;
        std::string bin_home;     // proposed-XDG, what uv uses
        std::string luban_prefix; // luban-specific umbrella override
    } xdg;

    // Home directory + USERPROFILE — what paths.cpp::home() resolved to.
    std::string home_dir;
};

// Take a snapshot. Cheap (~1ms typical); not cached, since callers usually
// invoke it once.
Host snapshot();

// JSON view with schema=1. Field names match the struct.
nlohmann::json to_json(const Host& h);

}  // namespace luban::perception
