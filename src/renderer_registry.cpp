// See `renderer_registry.hpp` for design rationale.
//
// Post-AH (DESIGN §24.1): this TU contains zero Lua C API code. The
// frontend ↔ core boundary is `RendererFns` (std::function pair). All
// Lua wiring lives in src/lua_frontend.cpp.

#include "renderer_registry.hpp"

#include <utility>

namespace luban::renderer_registry {

void RendererRegistry::register_native(std::string name,
                                        luban::render_types::RendererFns fns) {
    // Last-wins: assigning into the map drops the prior std::function value.
    // Lua-backed fns' shared_ptr<LuaRef> captures release on dtor — the
    // engine's refs are reclaimed automatically. No manual luaL_unref here
    // (that's the whole point of the AH boundary — this TU is Lua-free).
    native_[std::move(name)] = std::move(fns);
}

const luban::render_types::RendererFns*
RendererRegistry::find_native(std::string_view name) const {
    auto it = native_.find(std::string(name));
    if (it == native_.end()) return nullptr;
    return &it->second;
}

}  // namespace luban::renderer_registry
