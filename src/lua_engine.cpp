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
