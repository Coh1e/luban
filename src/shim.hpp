#pragma once
// luban writes one .cmd shim per alias into bin_dir() (= <data>/bin/, a
// luban-owned dir; cf. cargo's ~/.cargo/bin/ pattern). The .exe twin is
// hard-linked separately by the shim_cmd / component pipelines via
// luban-shim.exe.
//
// Earlier versions also wrote .ps1 and extensionless .sh shims; dropped in
// v0.2 — shells run .cmd fine and PATHEXT picks .exe for tooling.
//
// Target exe + prefix args are baked into the shim at install time — no env
// lookups at runtime, so the shim works in any shell.

#include <filesystem>
#include <string>
#include <vector>

namespace luban::shim {

namespace fs = std::filesystem;

// Outcome of a shim write attempt. `Skipped` means a non-luban file already
// occupies the path; caller should warn and move on (or pass force=true).
enum class WriteResult { Wrote, Skipped, Failed };

// Write <bin>/<alias>.cmd. If the .cmd already exists and `force` is false
// AND the alias is not already in the shim table (i.e., not one we own),
// returns Skipped without writing. Returns Wrote on success.
WriteResult write_shim(const std::string& alias,
                       const fs::path& exe,
                       const std::vector<std::string>& prefix_args = {},
                       bool force = false);

// Best-effort cleanup: removes <alias>.cmd, .exe, and any legacy .ps1 / sh
// twins from prior versions if they exist. Returns count actually removed.
int remove_shim(const std::string& alias);

// True iff `alias` appears in the current bin_dir/.shim-table.json. Used by
// install / shim rebuild paths to decide whether a write is "ours to overwrite"
// vs. "someone else's tool with the same name".
bool is_managed(const std::string& alias);

}  // namespace luban::shim
