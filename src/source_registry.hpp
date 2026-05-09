// `source_registry` — read/write of `<config>/sources.toml`, the user's
// list of registered blueprint sources (DESIGN §9.10).
//
// A source is a named pointer to a remote (or local) blueprint repository.
// Once registered, blueprints inside it are reachable as `<source>/<bp>`
// from `luban bp apply`. The registry lives in plain TOML so users can
// inspect / hand-edit / git-track their dotfiles via chezmoi or similar
// without a luban-internal database.
//
// Schema (§9.10.2):
//   [source.<name>]
//   url      = "https://github.com/<o>/<r>"   # or "file:///<abs-path>"
//   ref      = "main"                          # branch / tag / sha
//   commit   = "abc123..."                     # last-synced commit (or
//                                              #   "tarball:<iso8601>" for
//                                              #   non-git origins)
//   added_at = "2026-05-06T12:00:00Z"
//
// We keep the writer hand-rolled (not via toml++ output) so the on-disk
// format is byte-stable regardless of the toml++ version we're vendoring.

#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace luban::source_registry {

struct SourceEntry {
    std::string name;
    std::string url;
    std::string ref;       ///< branch / tag / sha; empty = repo default
    std::string commit;    ///< last-synced commit sha (or "tarball:<ts>")
    std::string added_at;  ///< ISO-8601 UTC of `bp source add`
    /// True when the source URL points at a known-official owner
    /// (DESIGN §8 trust model — official sources default-trust, still
    /// show summary; non-official require explicit consent at apply
    /// time and surface in `doctor`). Computed at `bp source add` time
    /// from `url` via `is_official_url`. Persisted so future schema
    /// changes to the official set don't silently re-classify already
    /// registered sources.
    bool official = false;
};

/// True iff `url` matches the official owner allowlist (DESIGN §8). The
/// allowlist is intentionally a hard-coded list of GitHub owners rather
/// than a remote-fetched registry — luban can't bootstrap trust from
/// the network it's about to fetch from.
bool is_official_url(std::string_view url);

/// Read the registry. Missing file → empty vector (not an error).
[[nodiscard]] std::expected<std::vector<SourceEntry>, std::string>
read_file(const std::filesystem::path& path);

/// Atomic write (tmp + rename), same pattern as blueprint_lock.
[[nodiscard]] std::expected<void, std::string>
write_file(const std::filesystem::path& path,
           const std::vector<SourceEntry>& entries);

/// Convenience: read from the canonical location (paths::bp_sources_registry_path()).
[[nodiscard]] std::expected<std::vector<SourceEntry>, std::string>
read();

/// Convenience: write back to the canonical location, ensuring the parent dir.
[[nodiscard]] std::expected<void, std::string>
write(const std::vector<SourceEntry>& entries);

/// Lookup helper. Returns nullopt if no entry has that name.
std::optional<SourceEntry> find(const std::vector<SourceEntry>& entries,
                                std::string_view name);

/// Validate a source name: alphanumeric / dash / underscore, 1..64 chars.
/// Used by `bp source add` to reject names that would collide with shell
/// metacharacters or filesystem oddities (mirrors git remote name rules).
bool is_valid_name(std::string_view name);

/// Reserved names per DESIGN §9.10.8: cannot be used by user-added sources.
/// `embedded` and `local` are conceptual namespaces; `main` is reserved
/// for the future default official source. Returns true if `name` is
/// reserved (i.e. should be rejected at `bp source add` time).
bool is_reserved_name(std::string_view name);

}  // namespace luban::source_registry
