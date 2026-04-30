#pragma once
// Compose env vars + PATH dirs that luban-spawned children need to find
// toolchains and respect XDG cache locations.
//
// Real toolchain bin dirs first; shim dir last (so cmake's compiler probe
// finds the real .exe before falling back to .cmd shim).
//
// History note: LUBAN_DATA/CACHE/STATE/CONFIG used to be written here too,
// for activate scripts. Activate scripts were removed when shims moved to
// ~/.local/bin (uv-style); these XDG-derived vars had no consumers and were
// pruned with them.

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace luban::env_snapshot {

namespace fs = std::filesystem;

std::vector<fs::path> path_dirs();

// Vars luban spawn-children should see beyond plain PATH:
//   - VCPKG_ROOT              → if vcpkg is installed (toolchain root);
//                               cmake's CMakePresets.json references this
//   - VCPKG_DOWNLOADS         → <cache>/vcpkg/downloads (XDG cache obeyed)
//   - VCPKG_DEFAULT_BINARY_CACHE → <cache>/vcpkg/archives
//   - X_VCPKG_REGISTRIES_CACHE   → <cache>/vcpkg/registries
//
// Returned as a list to preserve insertion order for deterministic output.
std::vector<std::pair<std::string, std::string>> env_dict();

// Returns env_overrides — a delta (the env_dict() entries + a prepended PATH)
// the caller can splice into a subprocess env to give it a luban-aware shell.
std::map<std::string, std::string> apply_to(const std::map<std::string, std::string>& env);

}  // namespace luban::env_snapshot
