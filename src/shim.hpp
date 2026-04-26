#pragma once
// 1:1 port of luban_boot/shim.py.
// Three siblings per alias: <bin>/<alias>.cmd, .ps1, and extensionless sh.
// Target exe + prefix args baked in at install time — no env lookups at
// runtime, so the shim works in any shell.

#include <filesystem>
#include <string>
#include <vector>

namespace luban::shim {

namespace fs = std::filesystem;

std::vector<fs::path> write_shim(const std::string& alias,
                                 const fs::path& exe,
                                 const std::vector<std::string>& prefix_args = {});

int remove_shim(const std::string& alias);

}  // namespace luban::shim
