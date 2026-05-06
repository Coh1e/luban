// `blueprint_lock` — read/write of `<blueprint>.lock` JSON files.
//
// A blueprint lock is the resolved, frozen view of a blueprint: every
// tool's exact platform-specific URL + sha256, every file's content hash.
// It's what makes `luban blueprint apply` reproducible across machines —
// the user commits the .lock alongside the .toml/.lua, and a fresh clone
// gets bit-identical resolution without re-hitting GitHub for "latest".
//
// Schema (DESIGN.md §9.4):
//   {
//     "schema": 1,
//     "blueprint_name": "...",
//     "blueprint_sha256": "<sha of source file>",
//     "resolved_at": "2026-05-05T...",
//     "tools": {
//       "<name>": {
//         "version": "...", "source": "...",
//         "platforms": {
//           "windows-x64": { "url": "...", "sha256": "...", "bin": "rg.exe",
//                            "artifact_id": "..." }
//         }
//       }
//     },
//     "files": {
//       "<target_path>": { "content_sha256": "...", "mode": "replace" }
//     }
//   }
//
// Writers: `commands/blueprint apply/update`. Readers: `apply/sync/build`.
// Both use this module so the JSON format stays consistent.

#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <unordered_map>

#include "blueprint.hpp"

namespace luban::blueprint_lock {

/// Resolved-platform record for a single tool. Mirror of PlatformSpec
/// but always populated (no optional fields) since the lock is post-resolve.
struct LockedPlatform {
    std::string url;
    std::string sha256;
    std::string bin;
    std::string artifact_id;
};

struct LockedTool {
    std::string version;
    std::string source;
    std::unordered_map<std::string, LockedPlatform> platforms;  // keyed by target
};

struct LockedFile {
    std::string content_sha256;
    luban::blueprint::FileMode mode;
};

struct BlueprintLock {
    int schema = 1;
    std::string blueprint_name;
    std::string blueprint_sha256;
    std::string resolved_at;  ///< ISO-8601 UTC timestamp.
    // DESIGN §9.10.6: when a blueprint came from a registered bp source
    // these record where + when. Empty `bp_source` = embedded or user-local
    // path origin. Old locks (pre-v1.0+source) are read tolerantly; the
    // fields default to empty.
    std::string bp_source;          ///< source name (e.g. "personal")
    std::string bp_source_commit;   ///< commit sha at apply time
    std::unordered_map<std::string, LockedTool> tools;
    std::unordered_map<std::string, LockedFile> files;
};

/// Read a lock file. Missing file → unexpected; malformed → unexpected.
[[nodiscard]] std::expected<BlueprintLock, std::string>
read_file(const std::filesystem::path& path);

/// Same but from a JSON string (used in tests).
[[nodiscard]] std::expected<BlueprintLock, std::string>
read_string(std::string_view json_text);

/// Atomically write a lock file (tmp + rename) so a crash mid-write
/// doesn't leave a half-formed lock that subsequent runs would accept.
[[nodiscard]] std::expected<void, std::string>
write_file(const std::filesystem::path& path, const BlueprintLock& lock);

/// Serialize a lock to a JSON string. Used by tests + by write_file.
[[nodiscard]] std::string
to_string(const BlueprintLock& lock);

}  // namespace luban::blueprint_lock
