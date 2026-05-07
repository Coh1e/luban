// See `resolver_registry.hpp` for design rationale.
//
// Post-AH (DESIGN §24.1): zero Lua C API in this TU. All Lua wiring
// lives in src/lua_frontend.cpp.

#include "resolver_registry.hpp"

#include <utility>

namespace luban::resolver_registry {

void ResolverRegistry::register_native(std::string scheme,
                                        luban::resolver_types::ResolverFn fn) {
    native_[std::move(scheme)] = std::move(fn);
}

const luban::resolver_types::ResolverFn*
ResolverRegistry::find_native(std::string_view scheme) const {
    auto it = native_.find(std::string(scheme));
    if (it == native_.end()) return nullptr;
    return &it->second;
}

}  // namespace luban::resolver_registry
