// `renderer_registry` — per-apply lookup table for blueprint-registered
// config renderers (DESIGN §9.9 议题 M (d), Tier 1 of plan
// elegant-strolling-hammock.md).
//
// A Lua blueprint can call `luban.register_renderer(name, module)` to
// declare a custom renderer alongside the bp's spec table. At apply time
// the bp's engine stays alive across both the parse phase and the render
// phase, so the function references the user wrote can be re-invoked when
// luban gets to `[config.<name>]` blocks.
//
// Lifetime contract:
//
//   1. blueprint_apply constructs ONE Engine + ONE RendererRegistry per Lua
//      blueprint apply. The Engine is attached to the registry's pointer
//      stash via `attach_registry()` before the bp parse runs.
//   2. During parse, the bp's `luban.register_renderer(name, M)` calls
//      Engine's C api_register_renderer, which stores `M.target_path` +
//      `M.render` as luaL_refs into the engine's LUA_REGISTRYINDEX and
//      records the (name → refs) mapping here.
//   3. Render phase calls `find(name)`, gets the refs, pushes them onto
//      the engine's stack and pcall's them to produce target_path + content.
//   4. Apply ends → registry destructs → unref all entries (so the engine
//      doesn't leak refs if it survives somehow).
//
// TOML bps don't construct a registry — they have no way to call
// register_renderer (per DESIGN §9.9 "TOML 只能引用，不能注册"). config_renderer
// falls back to its existing builtin-embedded path for them.

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

struct lua_State;

namespace luban::renderer_registry {

/// One registered renderer: function refs into the engine's LUA_REGISTRYINDEX.
/// `L` is the engine the refs belong to — caller must use the SAME lua_State*
/// when invoking. Mismatching engines crashes (or at best returns garbage).
struct Entry {
    lua_State* L = nullptr;
    int target_path_ref = -1;   ///< LUA_NOREF if uninit; otherwise a luaL_ref
    int render_ref = -1;
};

class RendererRegistry {
public:
    RendererRegistry() = default;
    ~RendererRegistry();

    RendererRegistry(const RendererRegistry&) = delete;
    RendererRegistry& operator=(const RendererRegistry&) = delete;

    /// Register a renderer named `name` whose target_path + render functions
    /// are at the given LUA_REGISTRYINDEX refs (pre-luaL_ref'd by caller).
    /// Replaces any existing entry of the same name (last-wins semantics
    /// match what the user would expect from re-running register_renderer).
    void register_lua(std::string name, lua_State* L,
                      int target_path_ref, int render_ref);

    /// Look up a renderer by name. Returns nullopt if absent.
    [[nodiscard]] std::optional<Entry> find(std::string_view name) const;

    /// Whether any renderers are registered. Cheap shortcut for callers
    /// that want to skip the registry lookup entirely on TOML bps.
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

private:
    std::unordered_map<std::string, Entry> entries_;
};

}  // namespace luban::renderer_registry
