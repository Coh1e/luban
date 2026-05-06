// `lua_engine` — RAII wrapper around an embedded Lua 5.4 VM, with luban's
// sandbox + API surface installed at construction time.
//
// Why Lua: blueprints (`*.lua`) can express conditionals, composition, and
// platform branching that pure TOML can't (see plan §1: "TOML primary +
// Lua optional"). Per-tool config renderers (`templates/configs/<name>.lua`)
// also live here — same engine, different entry point.
//
// Sandbox philosophy: blueprints are user-authored config, not arbitrary
// scripts. We strip the parts of the standard library that touch the
// filesystem, spawn processes, or load native code; what's left is a pure
// computation environment with controlled access to host info via the
// `luban.*` table we inject. Anything that needs to *cause an effect*
// (download, write file, run cmake) is C++ on the luban side, called
// outside the VM.
//
// Threading: not safe to share one Engine across threads. Spin a fresh
// VM per blueprint evaluation if parallelism is needed (cheap — Lua VM
// init is sub-millisecond).

#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

// Forward-declare Lua's opaque state type to keep this header free of
// the Lua C headers (which define lots of macros). lua_engine.cpp pulls
// them in.
struct lua_State;

namespace luban::renderer_registry { class RendererRegistry; }
namespace luban::resolver_registry { class ResolverRegistry; }

namespace luban::lua {

/// One Lua VM with luban's sandbox + API installed.
///
/// Construction is the only moment the sandbox is set up — once a VM exists,
/// any code that runs in it (including `eval_string` / `eval_file`) sees the
/// restricted environment.
class Engine {
   public:
    /// Spin up a fresh Lua 5.4 VM, load the standard libs, apply the sandbox
    /// (strip dangerous APIs), and inject the `luban.*` table.
    /// Throws `std::runtime_error` on Lua allocation failure.
    Engine();

    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    Engine(Engine&& other) noexcept;
    Engine& operator=(Engine&& other) noexcept;

    /// Run a chunk of Lua source. Returns the top-of-stack result as a
    /// string ("nil" for nil, "true"/"false" for booleans, the string itself
    /// for strings, decimal for numbers, "table: 0x..." for tables).
    /// On error returns `unexpected(message)` with Lua's traceback merged.
    ///
    /// Use this for one-off expressions in tests and for blueprint
    /// `return { ... }` files where blueprint_lua.cpp will then walk the
    /// result table directly via lua_State*.
    std::expected<std::string, std::string> eval_string(std::string_view code);

    /// Same as eval_string but reads from a file.
    std::expected<std::string, std::string> eval_file(
        const std::filesystem::path& path);

    /// Underlying raw lua_State*. Borrowed handle — Engine still owns it.
    /// Use for low-level access (e.g., after eval_file, walk the resulting
    /// table). Do NOT call lua_close() on the returned pointer.
    lua_State* state() noexcept { return L_; }

    /// Wire `luban.register_renderer(name, module)` to deposit refs into
    /// `*reg` (which must outlive the Engine). Without this call, the Lua
    /// API exists but is a no-op — renders the API harmless on TOML-bp
    /// applies that happen to share an Engine for unrelated reasons.
    /// Idempotent: re-attaching to a different registry replaces the
    /// pointer in this engine's LUA_REGISTRYINDEX stash.
    void attach_registry(luban::renderer_registry::RendererRegistry* reg);

    /// Same as attach_registry but for `luban.register_resolver(scheme, fn)`.
    /// Independent from the renderer registry — both can be attached to
    /// the same Engine, neither requires the other.
    void attach_resolver_registry(luban::resolver_registry::ResolverRegistry* reg);

   private:
    lua_State* L_ = nullptr;
};

namespace ext {

// `luban.download` indirection. The default impl in lua_engine.cpp returns
// nil + "not available in this build" (matches source_resolver pattern: the
// network half lives in a separate TU we can exclude from luban-tests).
// src/lua_engine_download.cpp registers the real impl at static-init time.
//
// Signature mirrors lua_CFunction so we can plug it straight in.
using LuaCFn = int (*)(lua_State*);
void set_download_fn(LuaCFn fn);
LuaCFn download_fn();

}  // namespace ext

}  // namespace luban::lua
