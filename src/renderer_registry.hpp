// `renderer_registry` — per-apply lookup table for config renderers
// (DESIGN §9.9 议题 M (d) Tier 1; AH/AI dispatch boundary §24.1).
//
// A blueprint at parse time can call `luban.register_renderer(name, M)`
// to declare a custom renderer; that flows through lua_engine.cpp's
// api_register_renderer → lua_frontend::wrap_renderer_module → here as
// `register_native(name, RendererFns)`. At apply time the
// config_renderer::render_with_registry path looks up by name via
// `find_native` and invokes the std::function callbacks directly. The
// 5 builtin renderers (templates/configs/<X>.lua) are registered the
// same way at apply start (phase 6 of the AH/AI rollout — currently
// pending), so dispatch is single-path and bp-registered renderers can
// shadow builtins last-wins.
//
// Lifetime contract:
//   1. commands/blueprint.cpp constructs ONE Engine + ONE RendererRegistry
//      per apply. Engine is attached to the registry's pointer stash via
//      Engine::attach_registry().
//   2. During the bp's parse-in-engine phase, register_renderer C calls
//      drop RendererFns into this registry; for Lua-backed entries the
//      RendererFns capture a shared_ptr<LuaRef> tied to the Engine.
//   3. Render phase: render_with_registry → find_native → fns(...).
//   4. Apply ends → registry destructs → std::function copies drop →
//      shared_ptr<LuaRef> refcounts hit zero → engine refs reclaimed.

#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

#include "render_types.hpp"

namespace luban::renderer_registry {

class RendererRegistry {
public:
    RendererRegistry() = default;
    ~RendererRegistry() = default;

    RendererRegistry(const RendererRegistry&) = delete;
    RendererRegistry& operator=(const RendererRegistry&) = delete;

    /// Register a renderer by std::function pair. Frontend-agnostic — caller
    /// may have built `fns` from Lua refs (via `lua_frontend::wrap_*`), a
    /// pure C++ lambda (tests, future native plugins), or any other source.
    /// Replaces any existing entry of the same name; the previous fns are
    /// destroyed (Lua-backed ones get their refs unrefd via the
    /// shared_ptr<LuaRef> in their captures).
    void register_native(std::string name, luban::render_types::RendererFns fns);

    /// Look up a registered renderer by name. Returns a borrowed pointer
    /// that stays valid until the next mutating call on this registry —
    /// caller must not store it across other register_* calls.
    [[nodiscard]] const luban::render_types::RendererFns*
    find_native(std::string_view name) const;

    /// Whether any renderers are registered. Cheap shortcut for callers
    /// that want to skip the registry lookup entirely.
    [[nodiscard]] bool empty() const noexcept { return native_.empty(); }

private:
    std::unordered_map<std::string, luban::render_types::RendererFns> native_;
};

}  // namespace luban::renderer_registry
