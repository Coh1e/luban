// `file_deploy` — materializes [file."path"] blocks from a blueprint into
// the user's filesystem. Four modes per DESIGN.md §11.4:
//
//   - replace: write the inline content to the target path verbatim;
//     before writing, back up the existing file (if any) so rollback
//     can restore it. luban "owns" the target after this.
//
//   - drop-in: write to a sibling drop-in subdir (e.g. ~/.gitconfig.d/
//     instead of ~/.gitconfig). The user retains ownership of the
//     canonical file and is expected to [include] the drop-in. luban
//     never reads or writes the canonical file in this mode.
//
//   - merge:   read existing JSON file (or {} if missing), apply the
//     `content` payload as a JSON Merge Patch (RFC 7396), atomic write
//     back. Use case: WT settings.json themes section without clobbering
//     the rest of the file. Pre-existing file is backed up like replace
//     mode. (v0.2.0)
//
//   - append:  bracket `content` in a luban marker block keyed by bp name
//     (`# >>> luban:<bp> >>>` ... `# <<< luban:<bp> <<<`) and either
//     replace an existing identically-keyed block in place, or append at
//     end of file. Idempotent across re-applies. Pre-existing file is
//     backed up. Use case: profile.ps1 multi-bp coordination. (v0.2.0)
//
// The deploy module is the boundary between blueprint intent and the
// user's home dir. It expands "~/" via paths::home(), and it reports
// the exact paths it touched so generation.cpp can later rollback.

#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "blueprint.hpp"

namespace luban::file_deploy {

/// Result of a single deploy operation. Generation tracks these so a
/// later rollback knows what to restore.
struct DeployedFile {
    std::filesystem::path target_path;     ///< Where we wrote.
    std::string content_sha256;            ///< sha256 of the bytes we wrote.
    luban::blueprint::FileMode mode;
    std::optional<std::filesystem::path>
        backup_path;                       ///< If replace mode AND target
                                           ///< pre-existed, where the
                                           ///< original is parked. Empty
                                           ///< for drop-in or first-write.
};

/// Expand `~/` and `~user/` (POSIX) prefixes against paths::home(). All
/// other paths pass through unchanged. Not a security boundary — a
/// malicious blueprint can still write to absolute paths; that's a
/// review concern, not a sandbox one (blueprints are user-authored).
[[nodiscard]] std::filesystem::path expand_home(std::string_view raw);

/// Deploy one [file."path"] entry. `generation_id` keys the backup
/// directory — `<state>/backups/<generation_id>/<base64-of-target>` — so
/// rollback from any generation can find its origin file. `bp_name` is
/// required for `Append` mode (used as the marker block key); empty is
/// fine for other modes.
///
/// For Merge / Append, the snapshot stored is the **post-merge / post-
/// append** content — i.e. what we actually wrote. That keeps
/// blueprint_reconcile's rollback story uniform across modes.
///
/// Side effect: also writes a content-addressed snapshot of `spec.content`
/// (or, for Merge / Append, of the *resulting* file content) at
/// `<state>/file-store/<sha256>/content` (idempotent — same sha = same
/// bytes, so collisions are no-ops). This snapshot is what
/// `blueprint_reconcile` reads when rolling back to a generation that
/// referenced a file the current generation no longer has — the original
/// backup chain only preserves *prior* content, not *deployed* content.
[[nodiscard]] std::expected<DeployedFile, std::string> deploy(
    const luban::blueprint::FileSpec& spec, int generation_id,
    std::string_view bp_name = "");

/// Restore the original from a backup. Used by generation::rollback.
/// If `backup_path` is empty (no original existed at deploy time), the
/// target is removed instead.
[[nodiscard]] std::expected<void, std::string> restore(
    const DeployedFile& deployed);

/// Path of the content-addressed snapshot for a given sha256 hex digest.
/// Returned even if the file does not exist — callers check `fs::exists`
/// before reading. Used by blueprint_reconcile to recreate files that
/// were dropped between the current and target generations.
[[nodiscard]] std::filesystem::path content_store_path(std::string_view sha256_hex);

/// Re-deploy content from the content store to a target path. Used by
/// rollback when the destination generation referenced a file that the
/// current generation lacks (so file_deploy::restore can't find a backup
/// for it). Pre-existing target file at `target_path` is overwritten
/// without a backup — the rollback step is itself the inverse operation
/// and we don't want to leave a backup pointing at intermediate state.
[[nodiscard]] std::expected<void, std::string> recreate_from_content_store(
    std::string_view sha256_hex, const std::filesystem::path& target_path);

}  // namespace luban::file_deploy
