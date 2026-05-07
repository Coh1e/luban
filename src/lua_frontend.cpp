// See `lua_frontend.hpp` for design rationale.
//
// Implementation notes:
//
// - `LuaRef` is a tiny RAII handle: ctor stashes (L, ref); dtor calls
//   `luaL_unref(L, LUA_REGISTRYINDEX, ref)`. We capture it via
//   `shared_ptr<LuaRef>` inside the std::function so refcount-driven
//   cleanup happens naturally — the last std::function copy goes out
//   of scope, the LuaRef dies, the engine reclaims the slot.
//
// - `StackGuard` is a debug-only sanity net: in debug builds it asserts
//   that `lua_gettop(L)` is unchanged from ctor to dtor on the same
//   wrapped call. Lua C API stack-balance bugs surface immediately
//   instead of corrupting subsequent pcalls. In release it's a no-op
//   so production has zero overhead.
//
// - `push_ctx` (renderer) and `build_spec_table` (resolver) are the
//   only places that allocate Lua tables for callback arguments. They
//   assume the caller has already pushed the function onto the stack
//   (via lua_rawgeti).

#include "lua_frontend.hpp"

#include <cassert>
#include <memory>
#include <utility>

extern "C" {
#include "lauxlib.h"  // luaL_unref, luaL_ref, LUA_REGISTRYINDEX
#include "lua.h"
}

#include "lua_json.hpp"
#include "platform.hpp"
#include "store.hpp"

namespace luban::lua_frontend {

namespace {

namespace bp = luban::blueprint;
namespace bpl = luban::blueprint_lock;

/// RAII for a single LUA_REGISTRYINDEX entry. dtor unrefs.
struct LuaRef {
    lua_State* L;
    int ref;

    LuaRef(lua_State* L_in, int ref_in) noexcept : L(L_in), ref(ref_in) {}
    ~LuaRef() {
        if (L && ref >= 0) {
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
        }
    }
    LuaRef(const LuaRef&) = delete;
    LuaRef& operator=(const LuaRef&) = delete;
    LuaRef(LuaRef&&) = delete;
    LuaRef& operator=(LuaRef&&) = delete;
};

/// Debug-only stack-balance guard. In release this compiles to nothing
/// (the assert macro evaluates to (void)0 when NDEBUG is set).
class StackGuard {
   public:
    StackGuard(lua_State* L, int delta = 0) noexcept
        : L_(L), expected_(lua_gettop(L) + delta) {}
    ~StackGuard() noexcept {
        assert(lua_gettop(L_) == expected_ &&
               "lua C API stack imbalance — wrapped call left junk on the stack");
    }
    StackGuard(const StackGuard&) = delete;
    StackGuard& operator=(const StackGuard&) = delete;

   private:
    lua_State* L_;
    int expected_;
};

void push_ctx(lua_State* L, const luban::config_renderer::Context& ctx) {
    lua_newtable(L);
    lua_pushstring(L, ctx.home.string().c_str());
    lua_setfield(L, -2, "home");
    lua_pushstring(L, ctx.xdg_config.string().c_str());
    lua_setfield(L, -2, "xdg_config");
    lua_pushstring(L, ctx.blueprint_name.c_str());
    lua_setfield(L, -2, "blueprint_name");
    lua_pushstring(L, ctx.platform.c_str());
    lua_setfield(L, -2, "platform");
}

/// Pop the top-of-stack value as a string. Strict: rejects numbers
/// (Lua's lua_isstring would auto-convert; we don't want that for
/// renderer / resolver outputs — silent stringification masks bugs).
std::expected<std::string, std::string> pop_string_or_error(
    lua_State* L, std::string_view fn_name) {
    if (lua_type(L, -1) != LUA_TSTRING) {
        std::string actual = lua_typename(L, lua_type(L, -1));
        lua_pop(L, 1);
        return std::unexpected(std::string(fn_name) + " returned " + actual +
                               ", expected string");
    }
    size_t len = 0;
    const char* s = lua_tolstring(L, -1, &len);
    std::string out(s, len);
    lua_pop(L, 1);
    return out;
}

/// Push the function at `ref`, push (cfg, ctx), pcall(2,1,0). Caller
/// then harvests the single return via pop_string_or_error.
std::expected<std::string, std::string> call_renderer_ref(
    lua_State* L, int ref, std::string_view fn_name,
    const nlohmann::json& cfg,
    const luban::config_renderer::Context& ctx) {
    StackGuard guard(L);  // expects gettop unchanged on return
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    if (!lua_isfunction(L, -1)) {
        std::string actual = lua_typename(L, lua_type(L, -1));
        lua_pop(L, 1);
        return std::unexpected(std::string("registered ") + std::string(fn_name) +
                               " ref points to " + actual + ", expected function");
    }
    luban::lua_json::push(L, cfg);
    push_ctx(L, ctx);
    if (lua_pcall(L, 2, 1, 0) != LUA_OK) {
        std::string err = lua_tostring(L, -1);
        lua_pop(L, 1);
        return std::unexpected(std::string("error calling ") +
                               std::string(fn_name) + ": " + err);
    }
    return pop_string_or_error(L, fn_name);
}

void build_spec_table(lua_State* L, const bp::ToolSpec& spec) {
    lua_newtable(L);
    lua_pushstring(L, spec.name.c_str());
    lua_setfield(L, -2, "name");
    if (spec.source) {
        lua_pushstring(L, spec.source->c_str());
        lua_setfield(L, -2, "source");
    }
    if (spec.version) {
        lua_pushstring(L, spec.version->c_str());
        lua_setfield(L, -2, "version");
    }
    if (spec.bin) {
        lua_pushstring(L, spec.bin->c_str());
        lua_setfield(L, -2, "bin");
    }
}

/// Walk a {url, sha256, bin?} table at the top of the stack into a
/// LockedPlatform. Does not pop — caller owns the table.
std::expected<bpl::LockedPlatform, std::string> walk_resolver_result(
    lua_State* L, std::string_view tool_name) {
    if (!lua_istable(L, -1)) {
        std::string actual = lua_typename(L, lua_type(L, -1));
        return std::unexpected("registered resolver for `" +
                               std::string(tool_name) + "` returned " + actual +
                               ", expected table");
    }
    bpl::LockedPlatform lp;
    lua_getfield(L, -1, "url");
    if (lua_isstring(L, -1)) lp.url = lua_tostring(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, -1, "sha256");
    if (lua_isstring(L, -1)) lp.sha256 = lua_tostring(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, -1, "bin");
    if (lua_isstring(L, -1)) lp.bin = lua_tostring(L, -1);
    lua_pop(L, 1);
    if (lp.url.empty()) {
        return std::unexpected("registered resolver for `" +
                               std::string(tool_name) +
                               "` returned table without required `url` field");
    }
    return lp;
}

}  // namespace

luban::render_types::RendererFns wrap_renderer_module(
    lua_State* L, int target_path_ref, int render_ref) {
    auto tp_holder = std::make_shared<LuaRef>(L, target_path_ref);
    auto r_holder = std::make_shared<LuaRef>(L, render_ref);

    luban::render_types::RendererFns out;
    out.target_path = [tp_holder](const nlohmann::json& cfg,
                                   const luban::config_renderer::Context& ctx) {
        return call_renderer_ref(tp_holder->L, tp_holder->ref,
                                  "target_path", cfg, ctx);
    };
    out.render = [r_holder](const nlohmann::json& cfg,
                             const luban::config_renderer::Context& ctx) {
        return call_renderer_ref(r_holder->L, r_holder->ref,
                                  "render", cfg, ctx);
    };
    return out;
}

std::expected<luban::render_types::RendererFns, std::string>
wrap_embedded_module(lua_State* L, std::string_view module_source,
                     std::string_view chunkname) {
    // Load + execute the module source; expect it to return a table.
    if (luaL_loadbuffer(L, module_source.data(), module_source.size(),
                        std::string(chunkname).c_str()) != LUA_OK) {
        std::string err = lua_tostring(L, -1);
        lua_pop(L, 1);
        return std::unexpected("Lua syntax in " + std::string(chunkname) +
                               ": " + err);
    }
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        std::string err = lua_tostring(L, -1);
        lua_pop(L, 1);
        return std::unexpected("module init failed (" + std::string(chunkname) +
                               "): " + err);
    }
    if (!lua_istable(L, -1)) {
        std::string actual = lua_typename(L, lua_type(L, -1));
        lua_pop(L, 1);
        return std::unexpected("module " + std::string(chunkname) +
                               " must `return { target_path = ..., render = ... }`"
                               " (got " + actual + ")");
    }
    int module_idx = lua_gettop(L);

    // Extract M.target_path; luaL_ref pops the value off the top of the
    // stack — we push it via getfield then ref.
    lua_getfield(L, module_idx, "target_path");
    if (!lua_isfunction(L, -1)) {
        std::string actual = lua_typename(L, lua_type(L, -1));
        lua_pop(L, 2);  // pop non-function + module table
        return std::unexpected("module " + std::string(chunkname) +
                               ".target_path is " + actual + ", expected function");
    }
    int tp_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    lua_getfield(L, module_idx, "render");
    if (!lua_isfunction(L, -1)) {
        std::string actual = lua_typename(L, lua_type(L, -1));
        luaL_unref(L, LUA_REGISTRYINDEX, tp_ref);
        lua_pop(L, 2);
        return std::unexpected("module " + std::string(chunkname) +
                               ".render is " + actual + ", expected function");
    }
    int r_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    lua_pop(L, 1);  // pop the module table; refs survive in REGISTRYINDEX.

    return wrap_renderer_module(L, tp_ref, r_ref);
}

luban::resolver_types::ResolverFn wrap_resolver_fn(lua_State* L, int fn_ref) {
    auto holder = std::make_shared<LuaRef>(L, fn_ref);

    return [holder](const bp::ToolSpec& spec)
               -> std::expected<bpl::LockedPlatform, std::string> {
        lua_State* L2 = holder->L;
        StackGuard guard(L2);
        lua_rawgeti(L2, LUA_REGISTRYINDEX, holder->ref);
        if (!lua_isfunction(L2, -1)) {
            std::string actual = lua_typename(L2, lua_type(L2, -1));
            lua_pop(L2, 1);
            return std::unexpected("registered resolver ref for `" + spec.name +
                                   "` is " + actual +
                                   ", expected function (engine recycled?)");
        }
        build_spec_table(L2, spec);
        if (lua_pcall(L2, 1, 1, 0) != LUA_OK) {
            std::string err = lua_tostring(L2, -1);
            lua_pop(L2, 1);
            return std::unexpected("registered resolver for `" + spec.name +
                                   "` raised: " + err);
        }
        auto plat = walk_resolver_result(L2, spec.name);
        lua_pop(L2, 1);  // pop the result table
        if (!plat) return std::unexpected(plat.error());

        // Caller (source_resolver::resolve_with_registry) finalizes
        // bin/artifact_id around our return — we just hand back the raw
        // {url, sha256, bin?} for it to massage.
        return *plat;
    };
}

}  // namespace luban::lua_frontend
