// `xdg_shim` — write/remove .cmd shims (Windows) or symlinks (POSIX) under
// `~/.local/bin/` for blueprint-installed tools.
//
// Why a separate module from src/shim.cpp:
//   - shim.cpp targets paths::bin_dir() = `<data>/bin/`, the v0.x location
//     for shimmed tools. It also maintains a `.shim-table.json` index used
//     by the `luban-shim.exe` proxy for hardlinked .exe aliases.
//   - blueprint-applied tools live at paths::xdg_bin_home() = `~/.local/bin/`
//     (DESIGN.md invariant 5). They use plain text .cmd shims with no exe
//     proxy needed (PATH search picks the .cmd directly for shells; tools
//     using CreateProcessW with explicit .exe lookups go through component
//     install / scoop instead, not blueprints).
//   - Reconciling the two paths is its own piece of work (议题 to resolve
//     v0.x vs v1.0 shim location). Until then xdg_shim is the v1.0 path.
//
// Both `bp apply` and `bp rollback` (via `blueprint_reconcile`) call into
// this module so shim creation and deletion stay on one code path.

#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace luban::xdg_shim {

namespace fs = std::filesystem;

/// Write `~/.local/bin/<alias>.cmd` (Windows) or `~/.local/bin/<alias>`
/// symlink (POSIX) pointing at `exe`. Overwrites any existing entry —
/// blueprint apply is the authoritative writer for these paths.
/// Returns the absolute path that was written.
[[nodiscard]] std::expected<fs::path, std::string> write_cmd_shim(
    std::string_view alias, const fs::path& exe);

/// Delete a previously-written shim. `shim_path` is the absolute path
/// recorded in the generation file (ToolRecord.shim_path). Idempotent:
/// missing file is not an error.
/// On POSIX also removes any extensionless symlink twin.
[[nodiscard]] std::expected<void, std::string> remove_cmd_shim(
    const fs::path& shim_path);

}  // namespace luban::xdg_shim
