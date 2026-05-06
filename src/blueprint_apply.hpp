// `blueprint_apply` — the orchestrator that turns a parsed blueprint
// + a resolved lock file into a populated `~/.local/bin/` + deployed
// configs + a new `<state>/generations/N+1.json`.
//
// This is the v1.0 "bring it all together" layer. Inputs are S3-S5
// modules:
//   - blueprint_toml/lua → BlueprintSpec (S3)
//   - source_resolver → LockedTool (S3)
//   - store::fetch → on-disk artifact (S4)
//   - external_skip → skip when scoop/etc. already provides (S4)
//   - file_deploy → write [files] entries (S4)
//   - config_renderer → render [config.X] then file_deploy (S4+S5)
//   - generation::write → snapshot what we did (S6)
//
// What apply() does NOT do:
//   - parse the blueprint file (caller does it; tests don't need fs)
//   - decide WHICH lock file to use (caller passes BlueprintLock)
//   - run a doctor check (that's `commands/doctor.cpp`'s job)
//
// Shim writing to `~/.local/bin/` lives in src/xdg_shim.cpp (extracted
// out of this module so blueprint_reconcile can rebuild shims during
// rollback). The legacy src/shim.cpp writes to `<data>/bin/` for the
// v0.x scoop/component pipeline — both shim writers coexist until v0.x
// is fully retired.

#pragma once

#include <expected>
#include <string>

#include "blueprint.hpp"
#include "blueprint_lock.hpp"

namespace luban::blueprint_apply {

struct ApplyOptions {
    /// If true, log what would happen but don't actually fetch / write.
    /// Useful for status preflight.
    bool dry_run = false;
};

struct ApplyResult {
    int new_generation_id = 0;
    int tools_fetched = 0;
    int tools_external = 0;
    int files_deployed = 0;
};

/// Apply one blueprint to the host. Drives the full pipeline:
///   1. for each tool in `spec`:
///        - if external_skip says the tool is on PATH → record external
///        - else find the right platform in `lock`, fetch via store,
///          write a shim under ~/.local/bin/<bin>
///   2. for each [config.X] in `spec`:
///        - render via config_renderer
///        - deploy via file_deploy as drop-in
///   3. for each [files] in `spec`:
///        - deploy via file_deploy with the user-specified mode
///   4. write generation N+1 with everything we did, set current to N+1
///
/// Errors are surfaced as `unexpected(message)` and short-circuit the
/// pipeline — partial state may exist on disk but the generation file
/// is not promoted, so a re-run treats it as fresh.
[[nodiscard]] std::expected<ApplyResult, std::string> apply(
    const luban::blueprint::BlueprintSpec& spec,
    const luban::blueprint_lock::BlueprintLock& lock,
    const ApplyOptions& opts = {});

}  // namespace luban::blueprint_apply
