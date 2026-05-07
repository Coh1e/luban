// `resolver_registry` — per-apply lookup table for blueprint-registered
// source resolvers (DESIGN §9.9 议题 L, Tier 1 v0.4.x followup to
// renderer_registry).
//
// A Lua blueprint can call `luban.register_resolver(scheme, fn)` to declare
// a new `source = "<scheme>:..."` handler alongside the bp's spec. At apply
// time the bp's engine stays alive across parse + lock-resolve + render so
// the function reference remains callable when source_resolver::resolve()
// hits a tool spec whose source uses the registered scheme.
//
// Symmetric to renderer_registry — same lifetime story, same engine-affinity
// guarantee. The two registries are independent (different keys, different
// invocation contracts) but share the same Engine instance.

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "resolver_types.hpp"

struct lua_State;

namespace luban::resolver_registry {

/// One registered resolver: a single function ref. The fn signature
/// (called via lua_pcall) is `function(spec : table) → table` where
/// `spec` carries (name, source, version) and the return is a
/// LockedTool-shaped table { url, sha256, bin } that the C++ side maps
/// into a LockedPlatform for the host triplet.
///
/// **Deprecated** — kept for the legacy `register_lua` path during the
/// AH/AI staged migration (DESIGN §24.1). Phase 5 removes it. New code
/// uses `ResolverFn` via `register_native`.
struct Entry {
    lua_State* L = nullptr;
    int fn_ref = -1;   ///< LUA_NOREF if uninit; otherwise a luaL_ref
};

class ResolverRegistry {
public:
    ResolverRegistry() = default;
    ~ResolverRegistry();

    ResolverRegistry(const ResolverRegistry&) = delete;
    ResolverRegistry& operator=(const ResolverRegistry&) = delete;

    // ---- Legacy Lua-coupled API (phase-out target, see DESIGN §24.1 AH) --

    /// Register a resolver for `scheme` (e.g. "emsdk", "winget"). The fn
    /// at fn_ref is luaL_ref'd into L's LUA_REGISTRYINDEX. Replaces any
    /// existing entry for the same scheme — last wins (matches DESIGN's
    /// "同一注册表" promise; bp-registered schemes can shadow earlier
    /// registrations for the same name).
    void register_lua(std::string scheme, lua_State* L, int fn_ref);

    [[nodiscard]] std::optional<Entry> find(std::string_view scheme) const;

    // ---- AH-aligned API (std::function callback, frontend-agnostic) ------

    /// Register a resolver for `scheme` by std::function callback. Frontend-
    /// agnostic — `fn` may wrap a Lua ref (via `lua_frontend::wrap_resolver_fn`),
    /// a pure C++ lambda, or any other implementation.
    void register_native(std::string scheme, luban::resolver_types::ResolverFn fn);

    /// Look up a native resolver by scheme. Borrowed pointer; valid until
    /// the next mutating call on this registry.
    [[nodiscard]] const luban::resolver_types::ResolverFn*
    find_native(std::string_view scheme) const;

    [[nodiscard]] bool empty() const noexcept {
        return entries_.empty() && native_.empty();
    }

private:
    std::unordered_map<std::string, Entry> entries_;
    std::unordered_map<std::string, luban::resolver_types::ResolverFn> native_;
};

}  // namespace luban::resolver_registry
