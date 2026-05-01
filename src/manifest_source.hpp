#pragma once
// Where component manifests come from since v0.2.
//
// Pre-v0.2 there was a `bucket_sync` module that, on cache miss, fetched
// from raw.githubusercontent.com/ScoopInstaller/{Main,Extras}/master/...
// That meant which version of cmake/ninja/etc. luban installed was
// silently slaved to whatever Scoop's master branch had today — bad for
// reproducibility, surprising for users, and a supply-chain surface area
// for the project to track.
//
// New rule: ONLY two sources, both vendored / under the user's control:
//
//   1. <data>/registry/overlay/<name>.json
//      Populated on first `luban setup` by selection::deploy_overlays(),
//      which copies every JSON in <repo>/manifests_seed/ into this dir.
//      Also where users drop hand-written manifests to override.
//
//   2. <seed_root>/<name>.json
//      Direct fallback — read straight from the in-tree manifests_seed/
//      dir locatable next to luban.exe. This makes `luban setup` work
//      even if overlay deployment hasn't happened yet (e.g., a fresh
//      LUBAN_PREFIX where <data> doesn't exist).
//
// No network. If a component's manifest isn't in either source, install
// fails with a clear error pointing the user to add it to manifests_seed/.

#include <filesystem>
#include <optional>
#include <string>

#include "json.hpp"

namespace luban::manifest_source {

namespace fs = std::filesystem;

// Where the manifest came from — surfaced for logging and `describe`.
struct LoadResult {
    nlohmann::json manifest;       // parsed
    fs::path path;                 // on-disk source we read
    std::string source_label;      // "overlay" or "seed"
};

// Look up `name`. Returns nullopt if not in overlay AND not in seed_root().
// Errors during JSON parse are logged and treated as a miss (rather than
// crashing the install pipeline on a malformed file we don't own).
std::optional<LoadResult> load(const std::string& name);

}  // namespace luban::manifest_source
