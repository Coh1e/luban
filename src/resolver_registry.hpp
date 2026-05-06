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

struct lua_State;

namespace luban::resolver_registry {

/// One registered resolver: a single function ref. The fn signature
/// (called via lua_pcall) is `function(spec : table) → table` where
/// `spec` carries (name, source, version) and the return is a
/// LockedTool-shaped table { url, sha256, bin } that the C++ side maps
/// into a LockedPlatform for the host triplet.
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

    /// Register a resolver for `scheme` (e.g. "emsdk", "winget"). The fn
    /// at fn_ref is luaL_ref'd into L's LUA_REGISTRYINDEX. Replaces any
    /// existing entry for the same scheme — last wins (matches DESIGN's
    /// "同一注册表" promise; bp-registered schemes can shadow earlier
    /// registrations for the same name).
    void register_lua(std::string scheme, lua_State* L, int fn_ref);

    [[nodiscard]] std::optional<Entry> find(std::string_view scheme) const;
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

private:
    std::unordered_map<std::string, Entry> entries_;
};

}  // namespace luban::resolver_registry
