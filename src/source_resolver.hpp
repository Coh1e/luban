// `source_resolver` — turn a ToolSpec's abstract `source` (e.g.
// "github:owner/repo") into a concrete LockedTool with platform-specific
// URLs + sha256s.
//
// Two modes (DESIGN.md §9.5):
//
//   1. **Inline** (manual): the user has filled in `[[tool.X.platform]]`
//      blocks themselves. resolve() copies them into the LockedTool
//      verbatim. No network hit.
//
//   2. **Auto**: `source = "github:owner/repo"`. resolve() asks GitHub's
//      releases API for the latest release, picks platform assets by
//      naming convention, downloads + hashes them. Network-dependent;
//      slower; should be cached in the lock file so subsequent builds
//      stay offline.
//
// In v1 we land **mode 1 only** (inline pass-through). The `github:`
// resolver is stubbed with a clear "not yet implemented" error so users
// who try it get a useful message rather than a silent miss. Wiring up
// real GitHub release queries comes in a later step (it needs an
// HTTP-with-headers helper and an asset-naming heuristic — both worth
// their own commits).

#pragma once

#include <expected>
#include <string>

#include "blueprint.hpp"
#include "blueprint_lock.hpp"

namespace luban::resolver_registry { class ResolverRegistry; }

namespace luban::source_resolver {

/// Take a parsed ToolSpec and produce a LockedTool by either
/// (a) copying user-supplied platform blocks, or
/// (b) resolving via the source scheme.
///
/// Returns the LockedTool ready to drop into BlueprintLock.tools[name].
[[nodiscard]] std::expected<luban::blueprint_lock::LockedTool, std::string>
resolve(const luban::blueprint::ToolSpec& spec);

/// Same as resolve(), but consults a bp-registered ResolverRegistry first
/// (Tier 1, DESIGN §9.9). If `registry` knows the source scheme, the bp's
/// Lua function is invoked with `spec` and its return is mapped to a
/// LockedTool. Otherwise falls through to the C++ scheme dispatch
/// (github / pwsh-module / ...).
///
/// `registry` must outlive the call. Pass nullptr to behave identically
/// to plain `resolve()`.
[[nodiscard]] std::expected<luban::blueprint_lock::LockedTool, std::string>
resolve_with_registry(
    const luban::blueprint::ToolSpec& spec,
    const luban::resolver_registry::ResolverRegistry* registry);

/// Convenience: parse the leading scheme from a source string. Returns
/// e.g. "github" for "github:BurntSushi/ripgrep". Empty string for
/// schemes without a colon, or unrecognized formats.
///
/// Exposed so tests can probe the parser without going through resolve().
[[nodiscard]] std::string source_scheme(std::string_view source);

/// Convenience: parse the body part of a source string. For
/// "github:BurntSushi/ripgrep" returns "BurntSushi/ripgrep".
[[nodiscard]] std::string source_body(std::string_view source);

namespace detail {
/// Registration hooks used by source_resolver_<scheme>.cpp TUs to wire
/// their implementations in at static-init time. Production luban.exe
/// links those TUs, populating the pointers; tests don't, leaving them
/// null. See source_resolver.cpp for how `resolve()` consumes them.
using GithubResolverFn =
    std::expected<luban::blueprint_lock::LockedTool, std::string> (*)(
        const luban::blueprint::ToolSpec&);
void set_github_resolver(GithubResolverFn fn);

using PwshModuleResolverFn = GithubResolverFn;
void set_pwsh_module_resolver(PwshModuleResolverFn fn);
}  // namespace detail

}  // namespace luban::source_resolver
