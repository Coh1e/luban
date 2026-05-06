// See `resolver_registry.hpp` for design rationale.

#include "resolver_registry.hpp"

extern "C" {
#include "lauxlib.h"  // luaL_unref, LUA_REGISTRYINDEX
#include "lua.h"
}

namespace luban::resolver_registry {

ResolverRegistry::~ResolverRegistry() {
    for (auto& [_, e] : entries_) {
        if (e.L && e.fn_ref >= 0) {
            luaL_unref(e.L, LUA_REGISTRYINDEX, e.fn_ref);
        }
    }
}

void ResolverRegistry::register_lua(std::string scheme, lua_State* L,
                                     int fn_ref) {
    auto it = entries_.find(scheme);
    if (it != entries_.end()) {
        if (it->second.L && it->second.fn_ref >= 0) {
            luaL_unref(it->second.L, LUA_REGISTRYINDEX, it->second.fn_ref);
        }
        it->second = Entry{L, fn_ref};
        return;
    }
    entries_.emplace(std::move(scheme), Entry{L, fn_ref});
}

std::optional<Entry> ResolverRegistry::find(std::string_view scheme) const {
    auto it = entries_.find(std::string(scheme));
    if (it == entries_.end()) return std::nullopt;
    return it->second;
}

}  // namespace luban::resolver_registry
