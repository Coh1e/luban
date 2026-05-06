// `generation` — nix-profile-style snapshots of luban-managed state.
//
// Every `luban blueprint apply` / `unapply` writes a new generation
// file at <state>/generations/<N>.json. A separate <state>/current.txt
// holds the integer N of the active generation. Atomic switch is just
// rewriting current.txt (tmp + rename); rollback is rewriting it to
// any prior N. The generation files themselves are immutable once
// written.
//
// Why a text file rather than a symlink (like nix does):
//  - Windows symlinks need SeCreateSymbolicLinkPrivilege (admin or
//    dev mode), which we explicitly avoid (DESIGN.md invariant 6).
//  - A text file with one integer is simpler, portable, and atomic
//    via rename in std::filesystem.
//
// What goes in a generation:
//  - which blueprints are active (so unapply / rollback can identify
//    the contributors of a deployed item)
//  - one record per tool: artifact_id (or external_path if scoop/etc.
//    provided it), shim_path, source blueprint
//  - one record per file: target_path, content_sha256, mode, backup_path
//
// Adding a tool/file → write new generation N+1 with N's contents +
// the addition. Removing → write N+1 with the removal.
//
// See DESIGN.md §12 for the model and §15 (events) for the lifecycle.

#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "blueprint.hpp"

namespace luban::generation {

/// One tool entry in a generation.
struct ToolRecord {
    std::string from_blueprint;            ///< Blueprint that introduced this tool.
    std::string artifact_id;               ///< Empty when is_external == true.
    std::string shim_path;                 ///< Absolute path to the primary shim
                                           ///< written into ~/.local/bin/.
                                           ///< Empty when is_external (luban
                                           ///< wrote no shim).
    bool is_external = false;              ///< True if tool was already on
                                           ///< PATH from another source
                                           ///< (scoop/brew/system pkg).
    std::string external_path;             ///< When is_external, where it
                                           ///< was found.

    /// Path of the primary binary inside the artifact tree, relative to
    /// the artifact root in the store. Set during apply so rollback can
    /// re-shim a tool whose blueprint was unapplied: store_path(artifact_id)
    /// + bin_path_rel reconstructs the absolute exe path. Empty for legacy
    /// records (rollback must skip with a warning) and for is_external.
    std::string bin_path_rel;

    /// Additional shims for tools whose blueprint declared `shims = [...]`
    /// (multi-binary tools — git-for-windows, etc.). Each entry is the
    /// absolute path of a `.cmd` written under ~/.local/bin/. Parallel to
    /// `bin_paths_rel_secondary` (same length, same order). The primary
    /// shim is still in `shim_path` / `bin_path_rel` for backward compat.
    std::vector<std::string> shim_paths_secondary;
    std::vector<std::string> bin_paths_rel_secondary;
};

/// One file entry in a generation.
struct FileRecord {
    std::string from_blueprint;
    std::string target_path;
    std::string content_sha256;
    luban::blueprint::FileMode mode;
    std::optional<std::string> backup_path;
};

/// One generation snapshot.
struct Generation {
    int schema = 1;
    int id = 0;
    std::string created_at;                ///< ISO-8601 UTC.
    std::vector<std::string> applied_blueprints;
    std::unordered_map<std::string, ToolRecord> tools;  ///< keyed by tool name
    std::unordered_map<std::string, FileRecord> files;  ///< keyed by target_path
};

// ---- on-disk layout ------------------------------------------------------

/// `<state>/generations/`
[[nodiscard]] std::filesystem::path generations_dir();

/// `<state>/generations/<id>.json`
[[nodiscard]] std::filesystem::path generation_path(int id);

/// `<state>/current.txt` — single integer, the active generation id.
/// Missing file = no current generation (fresh install).
[[nodiscard]] std::filesystem::path current_path();

// ---- IO ------------------------------------------------------------------

/// Read a specific generation by id. Missing file → unexpected.
[[nodiscard]] std::expected<Generation, std::string> read(int id);

/// Atomic write of a generation file (tmp + rename). The id field of
/// the passed Generation is the destination filename's id. Does NOT
/// update current.txt — call set_current separately so writes can be
/// staged.
[[nodiscard]] std::expected<void, std::string> write(const Generation& gen);

/// Read the current generation id. Returns nullopt if current.txt is
/// missing or malformed (treated as "no current generation").
[[nodiscard]] std::optional<int> get_current();

/// Atomic update of current.txt to point at the given id. Caller must
/// ensure the matching generation file exists already.
[[nodiscard]] std::expected<void, std::string> set_current(int id);

/// Find the highest generation id present on disk. Returns 0 when no
/// generations exist (so `next = highest_id() + 1` is always correct).
[[nodiscard]] int highest_id();

/// List all generation ids on disk, sorted ascending.
[[nodiscard]] std::vector<int> list_ids();

// ---- helpers -------------------------------------------------------------

/// ISO-8601 UTC timestamp string for now(). Used to populate
/// Generation::created_at when minting a new one.
[[nodiscard]] std::string now_iso8601();

}  // namespace luban::generation
