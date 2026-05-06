// See `lua_engine.hpp` for design rationale.
//
// Sandbox layer is intentionally aggressive: anything that could read/write
// arbitrary files, spawn processes, load native code, or escape the VM is
// pulled out. The "leave it in and trust the user" school doesn't apply —
// blueprints get committed to dotfile repos, copied across machines, shared
// between users; we want them to be data, not vehicles for arbitrary code.
//
// API surface is intentionally tiny in this v1 scaffold: just enough to
// prove the embed works end-to-end (luban.version + luban.platform.os/arch
// + luban.env.get). Later weeks add luban.shell.which, luban.fs.read, etc.
// as blueprint_lua / config_renderer demand them.

#include "lua_engine.hpp"

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include "luban/version.hpp"
#include "paths.hpp"
#include "renderer_registry.hpp"
#include "resolver_registry.hpp"

namespace luban::lua {

namespace ext {
namespace {
LuaCFn g_download_fn = nullptr;
}  // namespace
void set_download_fn(LuaCFn fn) { g_download_fn = fn; }
LuaCFn download_fn() { return g_download_fn; }
}  // namespace ext

namespace {

// ---- sandbox: strip dangerous standard-library entries -----------------

// Set table-at-top.<name> = nil. Top of stack must be the table; stack
// layout is preserved.
void nil_field(lua_State* L, const char* name) {
    lua_pushnil(L);
    lua_setfield(L, -2, name);
}

// Set _G.<name> = nil.
void nil_global(lua_State* L, const char* name) {
    lua_pushnil(L);
    lua_setglobal(L, name);
}

void install_sandbox(lua_State* L) {
    // `io`: kill anything that touches the filesystem or pipes. We leave
    // `io.read` / `io.write` since they're the canonical "talk to the VM
    // host" channel and our tests want to assert via captured stdout —
    // but blueprints in production should not call them; that's policy,
    // not enforcement.
    //
    // Actually we DO strip them for v1 — blueprints have no business
    // printing to stdout. If a renderer needs to log, it returns a
    // structured value; luban host code logs.
    lua_getglobal(L, "io");
    if (lua_istable(L, -1)) {
        for (const char* name : {"open", "lines", "popen", "input", "output",
                                 "stdin", "stdout", "stderr", "read", "write",
                                 "close", "tmpfile", "flush", "type"}) {
            nil_field(L, name);
        }
    }
    lua_pop(L, 1);

    // `os`: strip process control + fs mutation; keep date/time/getenv
    // (read-only, deterministic-ish).
    lua_getglobal(L, "os");
    if (lua_istable(L, -1)) {
        for (const char* name : {"execute", "exit", "remove", "rename",
                                 "tmpname", "setlocale"}) {
            nil_field(L, name);
        }
    }
    lua_pop(L, 1);

    // `package`: kill native loader; keep `package.path` so we can later
    // sandbox-control `require` to look only inside the blueprint dir.
    lua_getglobal(L, "package");
    if (lua_istable(L, -1)) {
        for (const char* name : {"loadlib", "cpath", "searchpath"}) {
            nil_field(L, name);
        }
    }
    lua_pop(L, 1);

    // Top-level loaders that sidestep package.path policy.
    nil_global(L, "loadfile");
    nil_global(L, "dofile");

    // Debug library: introspection vectors (getinfo, sethook, getupvalue
    // patching). v1 just yanks the whole thing.
    nil_global(L, "debug");

    // `print` — blueprints shouldn't write to stdout. If we ever want a
    // log hook, expose it as `luban.log(level, msg)` instead.
    nil_global(L, "print");
}

// ---- luban.* API surface ------------------------------------------------

int api_platform_os(lua_State* L) {
#ifdef _WIN32
    lua_pushliteral(L, "windows");
#elif defined(__APPLE__)
    lua_pushliteral(L, "macos");
#elif defined(__linux__)
    lua_pushliteral(L, "linux");
#else
    lua_pushliteral(L, "unknown");
#endif
    return 1;
}

int api_platform_arch(lua_State* L) {
#if defined(_M_X64) || defined(__x86_64__)
    lua_pushliteral(L, "x64");
#elif defined(_M_ARM64) || defined(__aarch64__)
    lua_pushliteral(L, "arm64");
#elif defined(_M_IX86) || defined(__i386__)
    lua_pushliteral(L, "x86");
#else
    lua_pushliteral(L, "unknown");
#endif
    return 1;
}

// `luban.env.get(name)` — read environment variable; returns nil if unset.
// Note: on Windows we use std::getenv which returns ANSI; for v1 scaffold
// this is fine (USER, HOME, PATH etc. are ASCII). UTF-8 fix is TODO.
int api_env_get(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const char* val = std::getenv(name);
    if (val) {
        lua_pushstring(L, val);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

// `luban.tool_root(name)` — return where toolchain `name` lives on disk
// (whether installed or not). Mirrors paths::toolchain_dir so blueprints
// can compose stable, layout-aware paths without hard-coding XDG specifics.
int api_tool_root(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    auto p = ::luban::paths::toolchain_dir(name);
    std::string s = p.string();
    lua_pushlstring(L, s.data(), s.size());
    return 1;
}

// `luban.download(url, [sha256])` — dispatched through ext::download_fn
// indirection so this TU stays free of WinHTTP/libcurl/miniz transitive
// includes. Production builds link src/lua_engine_download.cpp which
// registers the real implementation at static-init; luban-tests doesn't,
// so the fallback below kicks in and any test that touches the API gets
// a deterministic "not available" error.
int api_download(lua_State* L) {
    if (auto fn = ext::download_fn()) return fn(L);
    lua_pushnil(L);
    lua_pushstring(L, "luban.download not available in this build "
                      "(test build excludes the network half)");
    return 2;
}

// Keys under which we stash registry pointers in the engine's
// LUA_REGISTRYINDEX. lightuserdata so retrieval is cheap (no string keys
// in hot paths).
constexpr const char* kRendererRegistryKey = "luban_renderer_registry_ptr";
constexpr const char* kResolverRegistryKey = "luban_resolver_registry_ptr";

// `luban.register_renderer(name, module_table)` — declare a custom config
// renderer for `[config.<name>]` blocks in the bp. `module_table` must
// have function fields `target_path` and `render`, both with signature
// `(cfg : table, ctx : table) → string`. See DESIGN §9.9 inline registration.
//
// Without an attached registry (TOML-bp apply, programmatic engine, etc),
// this becomes a no-op so callers that defensively register_renderer can
// run in any context without errors.
int api_register_renderer(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    if (!lua_istable(L, 2)) {
        return luaL_error(L, "luban.register_renderer(\"%s\", module): module "
                              "must be a table {target_path = fn, render = fn}",
                          name);
    }

    // Validate fields BEFORE taking refs — failed validation shouldn't leak
    // a ref into LUA_REGISTRYINDEX.
    lua_getfield(L, 2, "target_path");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return luaL_error(L, "luban.register_renderer(\"%s\"): module.target_path "
                              "must be a function", name);
    }
    int tp_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_getfield(L, 2, "render");
    if (!lua_isfunction(L, -1)) {
        luaL_unref(L, LUA_REGISTRYINDEX, tp_ref);
        lua_pop(L, 1);
        return luaL_error(L, "luban.register_renderer(\"%s\"): module.render "
                              "must be a function", name);
    }
    int r_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    // Look up the registry pointer stashed by Engine::attach_registry.
    // Absent = no-op (silently drop the refs we just took since nobody
    // will ever call them).
    lua_pushstring(L, kRendererRegistryKey);
    lua_gettable(L, LUA_REGISTRYINDEX);
    auto* reg = static_cast<::luban::renderer_registry::RendererRegistry*>(
        lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!reg) {
        luaL_unref(L, LUA_REGISTRYINDEX, tp_ref);
        luaL_unref(L, LUA_REGISTRYINDEX, r_ref);
        return 0;
    }
    reg->register_lua(name, L, tp_ref, r_ref);
    return 0;
}

// `luban.register_resolver(scheme, fn)` — declare a custom source scheme.
// `fn(spec)` returns a LockedTool-shaped table {url, sha256, bin} for the
// host triplet. See DESIGN §9.9 inline registration. No-op when no
// ResolverRegistry is attached (mirrors register_renderer's behaviour).
int api_register_resolver(lua_State* L) {
    const char* scheme = luaL_checkstring(L, 1);
    if (!lua_isfunction(L, 2)) {
        return luaL_error(L, "luban.register_resolver(\"%s\", fn): fn must "
                              "be a function (received %s)",
                          scheme, lua_typename(L, lua_type(L, 2)));
    }
    // luaL_ref pops the value at top of stack — make sure that's the fn.
    lua_pushvalue(L, 2);
    int fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    lua_pushstring(L, kResolverRegistryKey);
    lua_gettable(L, LUA_REGISTRYINDEX);
    auto* reg = static_cast<::luban::resolver_registry::ResolverRegistry*>(
        lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!reg) {
        luaL_unref(L, LUA_REGISTRYINDEX, fn_ref);
        return 0;
    }
    reg->register_lua(scheme, L, fn_ref);
    return 0;
}

void install_luban_api(lua_State* L) {
    lua_newtable(L);  // luban

    // luban.version — pulled from generated luban/version.hpp (configure_file
    // out of CMakeLists.txt's project(luban VERSION ...)).
    lua_pushlstring(L, ::luban::kLubanVersion.data(),
                    ::luban::kLubanVersion.size());
    lua_setfield(L, -2, "version");

    // luban.platform = { os = fn, arch = fn }
    lua_newtable(L);
    lua_pushcfunction(L, api_platform_os);
    lua_setfield(L, -2, "os");
    lua_pushcfunction(L, api_platform_arch);
    lua_setfield(L, -2, "arch");
    lua_setfield(L, -2, "platform");

    // luban.env = { get = fn }
    lua_newtable(L);
    lua_pushcfunction(L, api_env_get);
    lua_setfield(L, -2, "get");
    lua_setfield(L, -2, "env");

    // luban.tool_root(name) → <data>/toolchains/<name>
    lua_pushcfunction(L, api_tool_root);
    lua_setfield(L, -2, "tool_root");

    // luban.download(url, [sha256]) → { path, sha256, bytes } or nil+err
    lua_pushcfunction(L, api_download);
    lua_setfield(L, -2, "download");

    // luban.register_renderer(name, module) → register a custom config
    // renderer (DESIGN §9.9). No-op when no RendererRegistry is attached
    // to the engine — see Engine::attach_registry.
    lua_pushcfunction(L, api_register_renderer);
    lua_setfield(L, -2, "register_renderer");

    // luban.register_resolver(scheme, fn) → register a custom source
    // scheme handler. Mirror of register_renderer; same no-op semantics
    // when no ResolverRegistry attached.
    lua_pushcfunction(L, api_register_resolver);
    lua_setfield(L, -2, "register_resolver");

    lua_setglobal(L, "luban");
}

// Convert top-of-stack value to a string for return from eval_string.
// Lua's lua_tostring coerces numbers automatically, but tables/functions
// it returns NULL for; we emit a deterministic placeholder so test asserts
// are well-defined.
std::string top_as_string(lua_State* L) {
    if (lua_isnil(L, -1)) return "nil";
    if (lua_isboolean(L, -1)) return lua_toboolean(L, -1) ? "true" : "false";
    if (lua_isstring(L, -1) || lua_isnumber(L, -1)) {
        return lua_tostring(L, -1);
    }
    // Table / function / userdata — caller (e.g. blueprint_lua.cpp) walks
    // the actual structure; eval_string just returns Lua's repr.
    return luaL_tolstring(L, -1, nullptr);
}

}  // namespace

// ---- Engine impl --------------------------------------------------------

void Engine::attach_registry(::luban::renderer_registry::RendererRegistry* reg) {
    if (!L_) return;
    lua_pushstring(L_, kRendererRegistryKey);
    if (reg) {
        lua_pushlightuserdata(L_, reg);
    } else {
        lua_pushnil(L_);
    }
    lua_settable(L_, LUA_REGISTRYINDEX);
}

void Engine::attach_resolver_registry(
    ::luban::resolver_registry::ResolverRegistry* reg) {
    if (!L_) return;
    lua_pushstring(L_, kResolverRegistryKey);
    if (reg) {
        lua_pushlightuserdata(L_, reg);
    } else {
        lua_pushnil(L_);
    }
    lua_settable(L_, LUA_REGISTRYINDEX);
}

Engine::Engine() {
    L_ = luaL_newstate();
    if (L_ == nullptr) {
        // Allocator failure — only hit on truly OOM systems. Throw because
        // there's no useful recovery and `Engine` should be a hard
        // construction (RAII) precondition for any blueprint work.
        throw std::runtime_error("luaL_newstate() returned null");
    }
    luaL_openlibs(L_);
    install_sandbox(L_);
    install_luban_api(L_);
}

Engine::~Engine() {
    if (L_) {
        lua_close(L_);
        L_ = nullptr;
    }
}

Engine::Engine(Engine&& other) noexcept : L_(other.L_) { other.L_ = nullptr; }

Engine& Engine::operator=(Engine&& other) noexcept {
    if (this != &other) {
        if (L_) lua_close(L_);
        L_ = other.L_;
        other.L_ = nullptr;
    }
    return *this;
}

std::expected<std::string, std::string> Engine::eval_string(
    std::string_view code) {
    // luaL_loadbuffer + lua_pcall is the standard "compile + run" pair.
    // Chunkname helps error messages; "=" prefix tells Lua not to wrap it
    // in `[string ...]`.
    if (luaL_loadbuffer(L_, code.data(), code.size(), "=eval_string") !=
        LUA_OK) {
        std::string err = lua_tostring(L_, -1);
        lua_pop(L_, 1);
        return std::unexpected(err);
    }
    if (lua_pcall(L_, 0, 1, 0) != LUA_OK) {
        std::string err = lua_tostring(L_, -1);
        lua_pop(L_, 1);
        return std::unexpected(err);
    }
    std::string result = top_as_string(L_);
    lua_pop(L_, 1);
    return result;
}

std::expected<std::string, std::string> Engine::eval_file(
    const std::filesystem::path& path) {
    // luaL_loadfile reads the file via Lua's libc-backed loader. Since we
    // strip `loadfile` from the global env (sandbox), this C-side helper
    // is the only way blueprints get loaded — controlled entry point.
    std::string path_str = path.string();
    if (luaL_loadfile(L_, path_str.c_str()) != LUA_OK) {
        std::string err = lua_tostring(L_, -1);
        lua_pop(L_, 1);
        return std::unexpected(err);
    }
    if (lua_pcall(L_, 0, 1, 0) != LUA_OK) {
        std::string err = lua_tostring(L_, -1);
        lua_pop(L_, 1);
        return std::unexpected(err);
    }
    std::string result = top_as_string(L_);
    lua_pop(L_, 1);
    return result;
}

}  // namespace luban::lua
