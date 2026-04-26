#pragma once
// 1:1 port of luban_boot/env_snapshot.py.
// Real toolchain bin dirs first; shim dir last (so cmake's compiler probe
// finds the real .exe before falling back to .cmd shim).

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace luban::env_snapshot {

namespace fs = std::filesystem;

std::vector<fs::path> path_dirs();

// Insertion-ordered (DATA, CACHE, STATE, CONFIG) — matches Python dict order
// so generated activate scripts are byte-identical.
std::vector<std::pair<std::string, std::string>> env_dict();

// Returns env_overrides — a delta (LUBAN_DATA + the prepended PATH) the caller
// can splice into a subprocess env, or use to write an activation script.
std::map<std::string, std::string> apply_to(const std::map<std::string, std::string>& env);

}  // namespace luban::env_snapshot
