// See `qjs_engine.hpp` for design rationale.
//
// QuickJS, unlike Lua, doesn't ship a "kitchen-sink" stdlib at all by
// default — JS_NewContext gives you only the language core (Array / Object
// / String / Math / JSON / Date / RegExp / Promise / Map / Set / etc.).
// quickjs-libc.c is what adds Os.exec / Std.loadFile / etc., and we
// deliberately did NOT vendor that file (see third_party/quickjs.LICENSE).
// So our "sandbox" job here is much simpler than for Lua: nothing dangerous
// is on by default; we just need to install our gated luban.* host object
// and stop there.
//
// API surface mirrors lua_engine v1: luban.version + luban.platform.os/arch
// + luban.env.get. Future weeks may extend (luban.shell.which, etc.) once
// the corresponding C++ helpers exist; do that on the lua side first and
// mirror here.

#include "qjs_engine.hpp"

#include <cstdlib>
#include <stdexcept>
#include <string>

extern "C" {
#include "quickjs.h"
}

#include "luban/version.hpp"

namespace luban::qjs {

namespace {

// ---- luban.* API ---------------------------------------------------------

JSValue api_platform_os(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/,
                        JSValueConst* /*argv*/) {
#ifdef _WIN32
    return JS_NewString(ctx, "windows");
#elif defined(__APPLE__)
    return JS_NewString(ctx, "macos");
#elif defined(__linux__)
    return JS_NewString(ctx, "linux");
#else
    return JS_NewString(ctx, "unknown");
#endif
}

JSValue api_platform_arch(JSContext* ctx, JSValueConst /*this_val*/,
                          int /*argc*/, JSValueConst* /*argv*/) {
#if defined(_M_X64) || defined(__x86_64__)
    return JS_NewString(ctx, "x64");
#elif defined(_M_ARM64) || defined(__aarch64__)
    return JS_NewString(ctx, "arm64");
#elif defined(_M_IX86) || defined(__i386__)
    return JS_NewString(ctx, "x86");
#else
    return JS_NewString(ctx, "unknown");
#endif
}

JSValue api_env_get(JSContext* ctx, JSValueConst /*this_val*/, int argc,
                    JSValueConst* argv) {
    if (argc < 1) return JS_NULL;
    const char* name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    const char* val = std::getenv(name);
    JSValue result = val ? JS_NewString(ctx, val) : JS_NULL;
    JS_FreeCString(ctx, name);
    return result;
}

void install_luban_api(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue luban = JS_NewObject(ctx);

    // luban.version (string)
    JS_SetPropertyStr(
        ctx, luban, "version",
        JS_NewStringLen(ctx, ::luban::kLubanVersion.data(),
                        ::luban::kLubanVersion.size()));

    // luban.platform = { os: fn, arch: fn }
    JSValue platform = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, platform, "os",
                      JS_NewCFunction(ctx, api_platform_os, "os", 0));
    JS_SetPropertyStr(ctx, platform, "arch",
                      JS_NewCFunction(ctx, api_platform_arch, "arch", 0));
    JS_SetPropertyStr(ctx, luban, "platform", platform);

    // luban.env = { get: fn }
    JSValue env = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, env, "get",
                      JS_NewCFunction(ctx, api_env_get, "get", 1));
    JS_SetPropertyStr(ctx, luban, "env", env);

    JS_SetPropertyStr(ctx, global, "luban", luban);
    JS_FreeValue(ctx, global);
}

// ---- error / value helpers ----------------------------------------------

// Pull the pending exception off the context and format it as a string.
// Includes stack trace if available. After this call the exception state
// is cleared.
std::string drain_exception(JSContext* ctx) {
    JSValue exc = JS_GetException(ctx);
    const char* msg = JS_ToCString(ctx, exc);
    std::string result = msg ? msg : "(unknown error)";
    if (msg) JS_FreeCString(ctx, msg);

    // Try to append the stack trace (Error.stack) if present.
    // Note: QuickJS-NG's JS_IsError takes (val) not (ctx, val) — differs
    // from older Bellard API.
    if (JS_IsError(exc)) {
        JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
        if (!JS_IsUndefined(stack)) {
            const char* stack_str = JS_ToCString(ctx, stack);
            if (stack_str) {
                result += "\n";
                result += stack_str;
                JS_FreeCString(ctx, stack_str);
            }
        }
        JS_FreeValue(ctx, stack);
    }

    JS_FreeValue(ctx, exc);
    return result;
}

// Convert a JSValue to a printable string for return from eval_string.
// Strings come through verbatim; objects/arrays get JSON.stringify; numbers
// get coerced via the standard ToString conversion.
std::string value_to_string(JSContext* ctx, JSValueConst val) {
    // Special-case undefined/null because JS_ToCString turns them into
    // literal "undefined" / "null" — that's fine and what we want.
    const char* s = JS_ToCString(ctx, val);
    if (!s) {
        // ToString itself threw; salvage what we can.
        std::string err = drain_exception(ctx);
        return "(tostring failed: " + err + ")";
    }
    std::string out = s;
    JS_FreeCString(ctx, s);
    return out;
}

}  // namespace

// ---- Engine impl --------------------------------------------------------

Engine::Engine() {
    rt_ = JS_NewRuntime();
    if (!rt_) {
        throw std::runtime_error("JS_NewRuntime() returned null");
    }
    ctx_ = JS_NewContext(rt_);
    if (!ctx_) {
        JS_FreeRuntime(rt_);
        rt_ = nullptr;
        throw std::runtime_error("JS_NewContext() returned null");
    }
    install_luban_api(ctx_);
}

Engine::~Engine() {
    if (ctx_) {
        JS_FreeContext(ctx_);
        ctx_ = nullptr;
    }
    if (rt_) {
        JS_FreeRuntime(rt_);
        rt_ = nullptr;
    }
}

Engine::Engine(Engine&& other) noexcept : rt_(other.rt_), ctx_(other.ctx_) {
    other.rt_ = nullptr;
    other.ctx_ = nullptr;
}

Engine& Engine::operator=(Engine&& other) noexcept {
    if (this != &other) {
        if (ctx_) JS_FreeContext(ctx_);
        if (rt_) JS_FreeRuntime(rt_);
        rt_ = other.rt_;
        ctx_ = other.ctx_;
        other.rt_ = nullptr;
        other.ctx_ = nullptr;
    }
    return *this;
}

std::expected<std::string, std::string> Engine::eval_string(
    std::string_view code) {
    JSValue result =
        JS_Eval(ctx_, code.data(), code.size(), "<eval_string>", JS_EVAL_TYPE_GLOBAL);

    if (JS_IsException(result)) {
        std::string err = drain_exception(ctx_);
        JS_FreeValue(ctx_, result);
        return std::unexpected(err);
    }

    std::string out = value_to_string(ctx_, result);
    JS_FreeValue(ctx_, result);
    return out;
}

std::expected<std::string, std::string> Engine::eval_file(
    const std::filesystem::path& path) {
    // QuickJS doesn't have a built-in "load file" helper without quickjs-libc.
    // Read it ourselves; that also keeps the fs touch in C++ where the
    // sandbox philosophy is honored.
    FILE* f = std::fopen(path.string().c_str(), "rb");
    if (!f) {
        return std::unexpected("cannot open " + path.string());
    }
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::string buf;
    buf.resize(size);
    if (std::fread(buf.data(), 1, size, f) != static_cast<size_t>(size)) {
        std::fclose(f);
        return std::unexpected("short read on " + path.string());
    }
    std::fclose(f);

    std::string filename = path.string();
    JSValue result = JS_Eval(ctx_, buf.data(), buf.size(), filename.c_str(),
                             JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        std::string err = drain_exception(ctx_);
        JS_FreeValue(ctx_, result);
        return std::unexpected(err);
    }
    std::string out = value_to_string(ctx_, result);
    JS_FreeValue(ctx_, result);
    return out;
}

}  // namespace luban::qjs
