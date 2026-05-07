// `resolver_types` — pure-C++ callback shape for source resolvers
// (DESIGN §24.1 AH).
//
// A `ResolverFn` takes a tool spec (name + source + version + bin) and
// returns the resolved single-platform record (url + sha256 + bin) for
// the host triplet, or an error string. The registry stores these;
// source_resolver dispatches via them. core never sees the Lua / native
// distinction.
//
// Header-only. No Lua dependency.

#pragma once

#include <expected>
#include <functional>
#include <string>

#include "blueprint.hpp"
#include "blueprint_lock.hpp"

namespace luban::resolver_types {

/// One resolver call. Returns a fully-populated LockedPlatform (url, sha256,
/// bin) for the host triplet — caller composes the LockedTool around it
/// (version, source, target triplet, artifact_id).
using ResolverFn = std::function<
    std::expected<luban::blueprint_lock::LockedPlatform, std::string>(
        const luban::blueprint::ToolSpec& spec)>;

}  // namespace luban::resolver_types
