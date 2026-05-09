// `blueprint_apply` — the orchestrator that turns a parsed blueprint
// + a resolved lock file into a populated `~/.local/bin/` + deployed
// configs + entries in <state>/luban/applied.txt + owned-shims.txt.
//
// Inputs are S3-S5 modules:
//   - blueprint_lua → BlueprintSpec
//   - source_resolver → LockedTool
//   - store::fetch → on-disk artifact
//   - external_skip → skip when an existing PATH entry already provides
//   - file_deploy → write [files] entries + config render output
//   - config_renderer → render [config.X] then file_deploy
//   - applied_db → mark_applied + record_owned_shim
//
// What apply() does NOT do:
//   - parse the blueprint file (caller does it; tests don't need fs)
//   - decide WHICH lock file to use (caller passes BlueprintLock)
//   - run a doctor check (that's `commands/doctor.cpp`'s job)
//
// Shim writing to `~/.local/bin/` lives in src/xdg_shim.cpp.

#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>

#include "blueprint.hpp"
#include "blueprint_lock.hpp"

namespace luban::renderer_registry { class RendererRegistry; }

namespace luban::blueprint_apply {

struct ApplyOptions {
    /// If true, log what would happen but don't actually fetch / write.
    /// Useful for status preflight.
    bool dry_run = false;

    /// Root of the bp source repo this blueprint came from (the dir that
    /// contains `blueprints/`, `scripts/`, …). Used to resolve `bp:`
    /// prefix on `[tool.X] post_install` paths so a bp can ship a
    /// registration script alongside its blueprint files instead of
    /// having to inject it into the upstream artifact.
    ///
    /// Empty / unset = `bp:` paths fail with a clear error. Programmatic
    /// callers (tests, embedded apply paths) that don't go through
    /// commands/blueprint.cpp can leave this empty if they don't need
    /// `bp:`-style scripts.
    std::optional<std::filesystem::path> bp_source_root;

    /// Renderer registry for `[config.X]` block dispatch (DESIGN §9.9 +
    /// §24.1 AH/AI). When set, apply funnels every render through
    /// `config_renderer::render_with_registry`, which serves
    /// bp-registered RendererFns first then falls through to the
    /// builtin embedded path.
    ///
    /// nullptr (legacy programmatic callers) → apply uses the plain
    /// `render()` path, equivalent to a registry with no native entries.
    /// commands/blueprint.cpp always passes a non-null registry as of
    /// the AI single-path commit.
    luban::renderer_registry::RendererRegistry* renderer_registry = nullptr;

    /// True when the bp source is on the official allowlist (DESIGN §8).
    /// Defaults true so programmatic callers (tests, file-only fixtures)
    /// don't accidentally trigger the non-official confirmation prompt.
    /// `commands/blueprint.cpp` consults `source_registry::SourceEntry::
    /// official` and forwards the result here.
    bool source_official = true;

    /// User-facing source label rendered in the trust summary header
    /// ("bp source: <name>"). Empty = "(local / unknown)". Optional —
    /// the summary still prints with whatever it has.
    std::string bp_source_name;

    /// Auto-confirm the trust summary's prompt for non-official sources.
    /// Mirrors the `--yes` flag on `bp apply`. Has no effect for official
    /// sources (no prompt is shown either way).
    bool yes = false;
};

struct ApplyResult {
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
///        - render via config_renderer + deploy via file_deploy
///   3. for each [files] in `spec`:
///        - deploy via file_deploy with the user-specified mode
///   4. on success: append spec.name to <state>/luban/applied.txt and
///      every created shim path to owned-shims.txt
///
/// Errors are surfaced as `unexpected(message)` and short-circuit the
/// pipeline — partial state may exist on disk but applied.txt is not
/// updated, so a re-run treats it as a fresh attempt.
[[nodiscard]] std::expected<ApplyResult, std::string> apply(
    const luban::blueprint::BlueprintSpec& spec,
    const luban::blueprint_lock::BlueprintLock& lock,
    const ApplyOptions& opts = {});

}  // namespace luban::blueprint_apply
