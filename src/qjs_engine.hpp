// `qjs_engine` — RAII wrapper around an embedded QuickJS-NG VM, with luban's
// sandbox + API surface installed at construction time.
//
// Mirrors `lua_engine` in shape and intent. JavaScript blueprints (`*.js`)
// and program renderers (`templates/programs/<name>.js`) live here. Per
// docs/DESIGN.md §10.3, JS is a **second-class** scripting layer in luban:
// Lua's "table = DSL" syntax is the preferred form for blueprints, and all
// v1.0 built-in renderers are written in Lua. JS is provided for users who
// strongly prefer it; luban itself does not ship JS-side defaults.
//
// Sandbox philosophy is identical to lua_engine: blueprints are user-authored
// config, not arbitrary scripts. We expose pure-compute ECMAScript globals
// (Array / Object / String / Math / JSON / Date / RegExp) plus a minimal
// luban.* host object, and refuse anything that touches the filesystem,
// spawns processes, or loads native code. The standard library subset that
// QuickJS's "quickjs-libc.c" provides (Os.exec / Std.loadFile etc.) is NOT
// linked — luban implements its own gated equivalents via api_*.
//
// Threading: not safe to share one Engine across threads. JSRuntime is
// per-thread by design. Spin a fresh VM per blueprint evaluation if
// parallelism is needed.

#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

// Forward-declare QuickJS opaque types to keep this header free of the
// quickjs.h macros.
struct JSRuntime;
struct JSContext;

namespace luban::qjs {

/// One QuickJS-NG VM with luban's sandbox + API installed.
///
/// Construction is the only moment the sandbox is set up. After ctor, any
/// code that runs in this VM (eval_string / eval_file) sees the restricted
/// environment.
class Engine {
   public:
    /// Spin up a fresh JSRuntime + JSContext, install the sandbox + the
    /// `luban.*` global object. Throws `std::runtime_error` on alloc fail.
    Engine();

    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    Engine(Engine&& other) noexcept;
    Engine& operator=(Engine&& other) noexcept;

    /// Run a chunk of JavaScript source. Returns the result coerced to a
    /// string ("undefined", "null", numbers as decimal, strings verbatim,
    /// objects as JSON.stringify with `[circular]` fallback). On error,
    /// returns `unexpected(message)` with stack trace if available.
    std::expected<std::string, std::string> eval_string(std::string_view code);

    /// Same as eval_string but reads from a file. Useful for blueprint .js
    /// files; blueprint_lua.cpp's QJS counterpart will call this.
    std::expected<std::string, std::string> eval_file(
        const std::filesystem::path& path);

    /// Underlying raw JSContext*. Borrowed handle — Engine still owns it.
    /// Use for low-level access (e.g., walking the result of eval_file).
    /// Do NOT call JS_FreeContext on the returned pointer.
    JSContext* context() noexcept { return ctx_; }

   private:
    JSRuntime* rt_ = nullptr;
    JSContext* ctx_ = nullptr;
};

}  // namespace luban::qjs
