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

#include "render_types.hpp"

struct lua_State;

namespace luban::renderer_registry {

/// One registered renderer: function refs into the engine's LUA_REGISTRYINDEX.
/// `L` is the engine the refs belong to — caller must use the SAME lua_State*
/// when invoking. Mismatching engines crashes (or at best returns garbage).
///
/// **Deprecated** — kept for the legacy `register_lua` path during the
/// AH/AI staged migration (DESIGN §24.1). Phase 5 of the decoupling plan
/// removes it. New code uses `RendererFns` (see `register_native`).
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

    // ---- Legacy Lua-coupled API (phase-out target, see DESIGN §24.1 AH) --

    /// Register a renderer named `name` whose target_path + render functions
    /// are at the given LUA_REGISTRYINDEX refs (pre-luaL_ref'd by caller).
    /// Replaces any existing entry of the same name (last-wins semantics
    /// match what the user would expect from re-running register_renderer).
    void register_lua(std::string name, lua_State* L,
                      int target_path_ref, int render_ref);

    /// Look up a legacy Lua-backed renderer by name. Returns nullopt if absent.
    [[nodiscard]] std::optional<Entry> find(std::string_view name) const;

    // ---- AH-aligned API (std::function callback, frontend-agnostic) ------

    /// Register a renderer by std::function pair. Frontend-agnostic — caller
    /// may have built `fns` from Lua refs (via `lua_frontend::wrap_*`), a
    /// pure C++ lambda (tests, future native plugins), or any other source.
    /// Replaces any existing entry of the same name; the previous fns are
    /// destroyed (Lua-backed ones get their refs unrefd via the
    /// shared_ptr<LuaRef> in their captures).
    void register_native(std::string name, luban::render_types::RendererFns fns);

    /// Look up a native renderer by name. Returns a borrowed pointer that
    /// stays valid until the next mutating call on this registry — caller
    /// must not store it across other register_* calls.
    [[nodiscard]] const luban::render_types::RendererFns*
    find_native(std::string_view name) const;

    /// Whether any renderers are registered (in either map). Cheap shortcut
    /// for callers that want to skip the registry lookup entirely.
    [[nodiscard]] bool empty() const noexcept {
        return entries_.empty() && native_.empty();
    }

private:
    std::unordered_map<std::string, Entry> entries_;
    std::unordered_map<std::string, luban::render_types::RendererFns> native_;
};

}  // namespace luban::renderer_registry
