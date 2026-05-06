// `store` — content-addressable store under <data>/store/<artifact-id>/.
//
// Per DESIGN.md §11.2: every tool artifact luban downloads lives at a
// stable, immutable path keyed by name + version + target + content hash.
// "Immutable" means we never overwrite once extracted; a different
// binary (even same name+version+target) gets a different artifact_id
// because the sha256 prefix differs.
//
// The store is the layer between download/extract and the rest of the
// pipeline (shim writer, generation, etc.). Anything downstream that
// asks for "where is ripgrep 14.0.3 windows-x64?" gets a fully-qualified
// path here. Idempotent: call fetch() twice with the same args and the
// second one is a no-op cache hit.
//
// What the store DOESN'T do:
// - PATH wiring (that's shim.cpp / commands/shim_cmd.cpp's job)
// - Garbage collection (deferred to v1.x — `luban blueprint gc`)
// - Network proxying / mirror fallback (download.cpp's job)

#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace luban::store {

/// Compute the artifact_id string from its identifying inputs.
///
/// Format: `<name>-<version>-<target>-<hash8>` where hash8 is the first
/// 8 hex chars of the artifact's sha256. This deviates from DESIGN.md
/// §11.1's full formula (which also includes recipe_rev + abi_tag) — we
/// land the simpler form in v1 because blueprints don't yet expose those
/// extra dimensions. When they do, this function gains those parameters
/// and the hash widens; old artifact_ids stay valid because the dir name
/// is the lookup key.
///
/// `version` may be empty — substituted with "unversioned" so the result
/// stays parseable. `sha256` is the artifact's binary hash and must be
/// at least 8 hex chars; passes through verbatim into hash8.
[[nodiscard]] std::string compute_artifact_id(std::string_view name,
                                              std::string_view version,
                                              std::string_view target,
                                              std::string_view sha256);

/// Return the directory where this artifact_id lives in the store.
/// May or may not exist on disk; use `is_present` to check.
[[nodiscard]] std::filesystem::path store_path(std::string_view artifact_id);

/// True if the artifact is already extracted at the canonical location
/// AND has a valid marker file. Crash-safe: an interrupted fetch leaves
/// no marker, so the next fetch re-extracts.
[[nodiscard]] bool is_present(std::string_view artifact_id);

struct FetchOptions {
    /// Progress bar label. Defaults to artifact_id if empty.
    std::string label;
};

struct FetchResult {
    /// `<data>/store/<artifact-id>/` — directory containing the extracted
    /// tool tree.
    std::filesystem::path store_dir;
    /// `store_dir / bin` — the absolute path to the executable inside.
    std::filesystem::path bin_path;
    /// True when fetch hit the cache; false when we actually downloaded.
    bool was_already_present;
};

/// Download + verify (sha256) + extract a tool archive into the store.
///
/// Behavior:
/// - If `is_present(artifact_id)` is already true → no network, return
///   immediately with `was_already_present = true`.
/// - Otherwise: download to `<cache>/downloads/<artifact-id>.archive`,
///   verify sha256, extract into a tmp directory under `<data>/store/`,
///   rename atomically to the final `<data>/store/<artifact-id>/`, and
///   write `.store-marker.json` with provenance.
/// - On any failure mid-fetch, the partial tmp directory is removed.
///
/// `bin` is the path inside the extracted tree (e.g. `bin/rg.exe` or
/// just `rg.exe`); FetchResult.bin_path joins it onto store_dir.
[[nodiscard]] std::expected<FetchResult, std::string> fetch(
    std::string_view artifact_id, std::string_view url,
    std::string_view sha256, std::string_view bin,
    const FetchOptions& opts = {});

}  // namespace luban::store
