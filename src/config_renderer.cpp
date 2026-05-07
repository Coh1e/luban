// See `config_renderer.hpp`.
//
// Two flow stages:
//
//  1. resolve_source — find the Lua source for tool_name. User-level
//     override at <config>/luban/configs/<tool>.lua wins; otherwise
//     fall through to the embedded_configs namespace shipped with the
//     binary.
//
//  2. execute — spin a fresh sandboxed lua_engine::Engine, load the
//     module source, expect it to return a table with target_path and
//     render fields, push cfg + ctx, call them, and harvest the
//     resulting strings.
//
// We deliberately spin a NEW engine per render call rather than
// reusing one. Engines are cheap (~sub-ms init) and per-call isolation
// keeps state from leaking across blueprints — a bug-prone thing once
// users start writing custom renderers that touch globals.

#include "config_renderer.hpp"

#include <fstream>
#include <sstream>
#include <unordered_map>

extern "C" {
// render_with_source still spins a fresh per-call engine and calls the
// lua C API directly. Phase 6 of the AH/AI rollout migrates the 5
// builtins into the registry (via lua_frontend::wrap_embedded_module)
// and deletes render_with_source entirely; at that point this include
// goes too and invariant 9 holds for this TU.
#include "lauxlib.h"
#include "lua.h"
}

#include "lua_engine.hpp"
#include "lua_json.hpp"
#include "paths.hpp"
#include "renderer_registry.hpp"

// Embedded-configs namespace — these headers are emitted by
// cmake/embed_text.cmake from templates/configs/<X>.lua. The headers
// declare `inline constexpr const char* <tool>_lua` strings.
//
// Wrapped in a single inclusion guard: the build system arranges for
// the headers to exist by the time this TU is compiled. Adding a new
// tool means: drop a new templates/configs/<X>.lua, list it in
// CMakeLists.txt's embed_text foreach, and append the include here.
#include "luban/embedded_configs/git.hpp"
#include "luban/embedded_configs/bat.hpp"
#include "luban/embedded_configs/fastfetch.hpp"
#include "luban/embedded_configs/yazi.hpp"
#include "luban/embedded_configs/delta.hpp"

namespace luban::config_renderer {

namespace {

namespace fs = std::filesystem;

/// Compile-time lookup from tool name to its embedded Lua source.
/// Adding a new builtin: include the embedded header above, add a row
/// here. Linear search is fine — handful of entries, not a hot path.
const char* embedded_source(std::string_view tool) {
    if (tool == "git")        return luban::embedded_configs::git_lua;
    if (tool == "bat")        return luban::embedded_configs::bat_lua;
    if (tool == "fastfetch")  return luban::embedded_configs::fastfetch_lua;
    if (tool == "yazi")       return luban::embedded_configs::yazi_lua;
    if (tool == "delta")      return luban::embedded_configs::delta_lua;
    return nullptr;
}

struct ResolvedSource {
    std::string code;            ///< Full Lua source text.
    std::string chunkname;       ///< Lua chunk name, used in tracebacks.
};

/// Look up the Lua source for `tool`. User override wins; otherwise
/// fall through to the embedded copy. Returns unexpected if neither
/// exists.
std::expected<ResolvedSource, std::string> resolve_source(std::string_view tool) {
    // 1. User override.
    // paths::config_dir() already ends in "luban", so just append
    // "configs/<tool>.lua" — no need for an extra "luban" segment.
    fs::path user_path =
        paths::config_dir() / "configs" / (std::string(tool) + ".lua");
    std::error_code ec;
    if (fs::is_regular_file(user_path, ec)) {
        std::ifstream in(user_path);
        if (!in) {
            return std::unexpected("cannot read " + user_path.string());
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        return ResolvedSource{ss.str(), "@" + user_path.string()};
    }

    // 2. Embedded fallback.
    if (auto* code = embedded_source(tool)) {
        return ResolvedSource{std::string(code), "=embedded:" + std::string(tool)};
    }

    return std::unexpected(
        "no renderer for `" + std::string(tool) +
        "` (no <config>/luban/configs/" + std::string(tool) +
        ".lua and no builtin)");
}

/// Push `ctx` onto Lua's stack as a Lua table. Layout matches what
/// renderer modules expect to read.
void push_ctx(lua_State* L, const Context& ctx) {
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

/// Harvest a string return value from the top of the Lua stack and
/// pop it. Returns unexpected if the value isn't a string.
///
/// Uses `lua_type == LUA_TSTRING` rather than `lua_isstring` because
/// the latter accepts numbers (Lua auto-converts). For renderer return
/// values we want strict typing — a number bug shouldn't silently
/// stringify into a config file.
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

/// Execute `M.<fn_name>(cfg, ctx)` where M is the module table at the
/// given stack idx. Pushes the return value to the top of the stack;
/// caller is responsible for harvesting + popping.
std::expected<void, std::string> call_module_fn(lua_State* L,
                                                int module_idx,
                                                const char* fn_name,
                                                const nlohmann::json& cfg,
                                                const Context& ctx) {
    lua_getfield(L, module_idx, fn_name);
    if (!lua_isfunction(L, -1)) {
        std::string actual = lua_typename(L, lua_type(L, -1));
        lua_pop(L, 1);
        return std::unexpected(std::string("module field `") + fn_name +
                               "` is " + actual + ", expected function");
    }
    luban::lua_json::push(L, cfg);
    push_ctx(L, ctx);
    if (lua_pcall(L, 2, 1, 0) != LUA_OK) {
        std::string err = lua_tostring(L, -1);
        lua_pop(L, 1);
        return std::unexpected(std::string("error calling ") + fn_name +
                               ": " + err);
    }
    return {};
}

}  // namespace

std::expected<RenderResult, std::string> render_with_source(
    std::string_view lua_source, std::string_view chunk_name,
    const nlohmann::json& cfg, const Context& ctx) {
    luban::lua::Engine engine;
    lua_State* L = engine.state();

    // Load + run the module source. Expect it to return a table.
    if (luaL_loadbuffer(L, lua_source.data(), lua_source.size(),
                        std::string(chunk_name).c_str()) != LUA_OK) {
        std::string err = lua_tostring(L, -1);
        lua_pop(L, 1);
        return std::unexpected("Lua syntax: " + err);
    }
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        std::string err = lua_tostring(L, -1);
        lua_pop(L, 1);
        return std::unexpected("Lua module init failed: " + err);
    }
    if (!lua_istable(L, -1)) {
        std::string actual = lua_typename(L, lua_type(L, -1));
        lua_pop(L, 1);
        return std::unexpected("module must `return { target_path = ..., render = ... }` (got " +
                               actual + ")");
    }
    int module_idx = lua_gettop(L);

    // M.target_path(cfg, ctx) → string
    if (auto err = call_module_fn(L, module_idx, "target_path", cfg, ctx); !err) {
        lua_pop(L, 1);  // pop module table
        return std::unexpected(err.error());
    }
    auto tp = pop_string_or_error(L, "target_path");
    if (!tp) {
        lua_pop(L, 1);
        return std::unexpected(tp.error());
    }

    // M.render(cfg, ctx) → string
    if (auto err = call_module_fn(L, module_idx, "render", cfg, ctx); !err) {
        lua_pop(L, 1);
        return std::unexpected(err.error());
    }
    auto content = pop_string_or_error(L, "render");
    if (!content) {
        lua_pop(L, 1);
        return std::unexpected(content.error());
    }

    lua_pop(L, 1);  // pop module table
    return RenderResult{fs::path(*tp), *content};
}

std::expected<RenderResult, std::string> render(std::string_view tool_name,
                                                const nlohmann::json& cfg,
                                                const Context& ctx) {
    auto src = resolve_source(tool_name);
    if (!src) return std::unexpected(src.error());
    return render_with_source(src->code, src->chunkname, cfg, ctx);
}

// ---- bp-registered renderer dispatch (Tier 1, DESIGN §9.9 / §24.1 AH) --
//
// Post-AH: pure callback dispatch. `RendererFns` may be backed by Lua
// refs (via `lua_frontend::wrap_renderer_module`), a pure C++ lambda,
// or any future native plugin. core dispatch doesn't care.

std::expected<RenderResult, std::string> render_with_registry(
    const luban::renderer_registry::RendererRegistry& registry,
    std::string_view tool_name, const nlohmann::json& cfg,
    const Context& ctx) {
    if (auto* fns = registry.find_native(tool_name); fns) {
        auto tp = fns->target_path(cfg, ctx);
        if (!tp) return std::unexpected(tp.error());
        auto content = fns->render(cfg, ctx);
        if (!content) return std::unexpected(content.error());
        return RenderResult{fs::path(*tp), *content};
    }
    // Fall through to the builtin / user-override path. Phase 6 will
    // pre-load builtins into the registry and delete this fallback.
    return render(tool_name, cfg, ctx);
}

}  // namespace luban::config_renderer
