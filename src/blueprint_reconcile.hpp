// `blueprint_reconcile` — make on-disk state match a target generation.
//
// The reverse of `blueprint_apply::apply()`. Used by:
//   - `bp rollback [N]` — switch current → N and reconcile so shims and
//     deployed files actually match what generation N described.
//   - (future) `bp gc`, `bp update` cleanup paths.
//
// What the existing data model supports:
//   - `file_deploy::DeployedFile.backup_path` is the user content from
//     RIGHT BEFORE the deploy. Restoring it gives back the immediately-
//     prior generation's state for that path.
//   - `file_deploy::content_store_path(sha)` is the bytes that *were*
//     deployed. New as of v1.0 — see file_deploy.hpp.
//   - `ToolRecord.artifact_id` + new `bin_path_rel` reconstruct the
//     absolute exe path so we can rebuild a shim that was previously
//     deleted (e.g. by an `unapply` we're now rolling back through).
//
// Algorithm — backward chain walk:
//   For each adjacent generation pair (higher, lower) walking from
//   `current` down to `target` inclusive (lower = `target` for the last
//   step), undo what `higher` changed relative to `lower`:
//
//     - Files added or modified at higher → restore via backup_path
//       (puts back lower's content; deletes if no backup existed).
//     - Files dropped at higher (only present in lower) → recreate via
//       `file_deploy::recreate_from_content_store(lower.sha)`.
//     - Tools added or changed at higher → remove higher's shims.
//     - Tools dropped at higher → rebuild shim from lower's artifact_id
//       and bin_path_rel.
//
//   Each step's records carry the metadata needed to undo *that* step,
//   so multi-step rollback is just a sequence of single-step undos.
//
// What the algorithm CANNOT do:
//   - Reconstruct content for files dropped at higher when lower's
//     content_sha256 is missing or its content-store entry was gc'd.
//     (`bp gc` does not exist yet, so this only matters for legacy
//     generations written before the content store was added — those
//     surface as warnings, not hard failures.)
//   - Re-shim tools whose `bin_path_rel` is missing (legacy records).
//     Same disposition: warn, skip, continue.
//
// Reconcile does NOT modify `<state>/current.txt`. The caller (rollback)
// flips the pointer after a successful reconcile.

#pragma once

#include <expected>
#include <string>
#include <vector>

namespace luban::blueprint_reconcile {

struct ReconcileResult {
    int target_id = 0;
    int files_restored = 0;     ///< Files reverted via file_deploy::restore.
    int files_recreated = 0;    ///< Files re-deployed from content store.
    int shims_removed = 0;
    int shims_recreated = 0;
    std::vector<std::string> warnings;
};

/// Bring on-disk state in line with the generation identified by
/// `target_id`. Walks intermediate generations between `current` and
/// `target` and undoes each step. Errors during individual file/shim
/// operations are appended to `warnings` rather than aborted, so a
/// partially-corrupt state still gets the rest of the reconciliation.
[[nodiscard]] std::expected<ReconcileResult, std::string>
reconcile_to(int target_id);

}  // namespace luban::blueprint_reconcile
