// See `renderer_registry.hpp` for design rationale.

#include "renderer_registry.hpp"

extern "C" {
#include "lauxlib.h"  // luaL_unref, LUA_REGISTRYINDEX
#include "lua.h"
}

namespace luban::renderer_registry {

RendererRegistry::~RendererRegistry() {
    for (auto& [_, e] : entries_) {
        if (e.L && e.target_path_ref >= 0) {
            luaL_unref(e.L, LUA_REGISTRYINDEX, e.target_path_ref);
        }
        if (e.L && e.render_ref >= 0) {
            luaL_unref(e.L, LUA_REGISTRYINDEX, e.render_ref);
        }
    }
}

void RendererRegistry::register_lua(std::string name, lua_State* L,
                                     int target_path_ref, int render_ref) {
    // Last-wins: if `name` already exists, unref the old function refs so
    // they don't leak. This matches what users would intuit from
    // re-registering — prior definition is replaced.
    auto it = entries_.find(name);
    if (it != entries_.end()) {
        if (it->second.L && it->second.target_path_ref >= 0) {
            luaL_unref(it->second.L, LUA_REGISTRYINDEX, it->second.target_path_ref);
        }
        if (it->second.L && it->second.render_ref >= 0) {
            luaL_unref(it->second.L, LUA_REGISTRYINDEX, it->second.render_ref);
        }
        it->second = Entry{L, target_path_ref, render_ref};
        return;
    }
    entries_.emplace(std::move(name), Entry{L, target_path_ref, render_ref});
}

std::optional<Entry> RendererRegistry::find(std::string_view name) const {
    auto it = entries_.find(std::string(name));
    if (it == entries_.end()) return std::nullopt;
    return it->second;
}

}  // namespace luban::renderer_registry
