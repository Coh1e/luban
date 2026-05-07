// `resolver_registry` — per-apply lookup table for source resolvers
// (DESIGN §9.9 议题 L Tier 1; AH/AI dispatch boundary §24.1).
//
// A blueprint at parse time can call `luban.register_resolver(scheme, fn)`
// to declare a new `source = "<scheme>:..."` handler; that flows through
// lua_engine.cpp's api_register_resolver → lua_frontend::wrap_resolver_fn →
// here as `register_native(scheme, ResolverFn)`. At lock-resolve time
// source_resolver::resolve_with_registry looks up by scheme via
// `find_native` and invokes the std::function callback directly.
//
// Symmetric to renderer_registry — same lifetime story, same per-apply
// engine affinity for Lua-backed entries. The two registries are
// independent (different keys, different invocation contracts).

#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

#include "resolver_types.hpp"

namespace luban::resolver_registry {

class ResolverRegistry {
public:
    ResolverRegistry() = default;
    ~ResolverRegistry() = default;

    ResolverRegistry(const ResolverRegistry&) = delete;
    ResolverRegistry& operator=(const ResolverRegistry&) = delete;

    /// Register a resolver for `scheme` by std::function callback. Frontend-
    /// agnostic — `fn` may wrap a Lua ref (via `lua_frontend::wrap_resolver_fn`),
    /// a pure C++ lambda, or any other implementation. Replaces any
    /// existing entry for the same scheme — last wins (matches DESIGN's
    /// "同一注册表" promise; bp-registered schemes can shadow earlier
    /// registrations for the same name).
    void register_native(std::string scheme, luban::resolver_types::ResolverFn fn);

    /// Look up a registered resolver by scheme. Borrowed pointer; valid
    /// until the next mutating call on this registry.
    [[nodiscard]] const luban::resolver_types::ResolverFn*
    find_native(std::string_view scheme) const;

    [[nodiscard]] bool empty() const noexcept { return native_.empty(); }

private:
    std::unordered_map<std::string, luban::resolver_types::ResolverFn> native_;
};

}  // namespace luban::resolver_registry
