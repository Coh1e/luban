// `lua_frontend` — sole TU bridging Lua refs into the pure-C++ callback
// types that core dispatch consumes (DESIGN §24.1 AH).
//
// luban_frontend.cpp is the ONLY .cpp in the project (besides lua_engine.cpp,
// blueprint_lua.cpp, lua_json.cpp, and config_renderer.cpp pre-AH cleanup)
// allowed to include lua.h / lauxlib.h. core modules see only the
// `RendererFns` / `ResolverFn` types from render_types.hpp /
// resolver_types.hpp. This is the seam.
//
// Lifetime contract:
// - Each `wrap_*` takes a `lua_State*` plus integer luaL_ref'd indices
//   into LUA_REGISTRYINDEX. The wrapper returns a callable that closes
//   over a `shared_ptr<LuaRef>` — last copy of the std::function dies →
//   the LuaRef destructor calls `luaL_unref` on the engine.
// - The lua_State* MUST outlive any copy of the wrapper. Today
//   commands/blueprint.cpp arranges this naturally (Engine + registries
//   are function-local; registries destruct before Engine in reverse
//   construction order). If callers ever re-arrange, they own the
//   ordering.

#pragma once

#include <expected>
#include <string>
#include <string_view>

#include "render_types.hpp"
#include "resolver_types.hpp"

// Forward-declare Lua's opaque state to keep this header lua.h-free —
// matches lua_engine.hpp's policy.
struct lua_State;

namespace luban::lua_frontend {

/// Wrap two LUA_REGISTRYINDEX refs (target_path, render) into a
/// `RendererFns` whose call sites invoke them via lua_pcall. The refs
/// are owned by the returned object — when the last copy dies, both
/// luaL_unref'd back to the engine.
[[nodiscard]] luban::render_types::RendererFns wrap_renderer_module(
    lua_State* L, int target_path_ref, int render_ref);

/// Wrap a single LUA_REGISTRYINDEX ref to a Lua function into a
/// `ResolverFn`. The fn is invoked with a `spec` table {name, source,
/// version, bin}; the return must be a table {url, sha256, bin?}.
[[nodiscard]] luban::resolver_types::ResolverFn wrap_resolver_fn(
    lua_State* L, int fn_ref);

/// Load a renderer module from Lua source (`return { target_path = ...,
/// render = ... }`), luaL_ref both fields into `L`'s LUA_REGISTRYINDEX,
/// and return a `RendererFns` that owns the refs via shared_ptr<LuaRef>.
///
/// Used by commands/blueprint.cpp to pre-load the 5 embedded builtins
/// (templates/configs/<X>.lua) into a registry at apply start so they
/// dispatch through the same registry path as bp-registered renderers
/// (DESIGN §9.9 line 656 "无双码路径", §24.1 AI).
///
/// Errors (Lua syntax / module-init pcall failure / missing or wrong-
/// typed M.target_path / M.render fields) propagate via the unexpected
/// channel — the caller decides whether to refuse the apply or skip
/// this builtin.
[[nodiscard]] std::expected<luban::render_types::RendererFns, std::string>
wrap_embedded_module(lua_State* L, std::string_view module_source,
                     std::string_view chunkname);

}  // namespace luban::lua_frontend
