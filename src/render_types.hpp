// `render_types` — pure-C++ callback shape for config renderers (DESIGN
// §24.1 AH "core ↔ Lua 解耦边界").
//
// One `RendererFns` is the contract a registered renderer satisfies:
// two callables that take (cfg, ctx) and return either a path string
// (for target_path) or content bytes (for render). The registry stores
// these; the apply pipeline calls them. Neither side knows or cares that
// the actual implementation may be backed by Lua refs (via lua_frontend),
// a pure C++ lambda (in tests), or a future native plugin.
//
// Header-only — no .cpp counterpart, no Lua dependency. core (config_renderer
// / blueprint_apply / renderer_registry) include this directly; the Lua
// adapter lives in src/lua_frontend.{hpp,cpp}.

#pragma once

#include <expected>
#include <functional>
#include <string>

#include "config_renderer.hpp"
#include "json.hpp"

namespace luban::render_types {

/// Contract: each function takes the [config.X] block (cfg) plus per-apply
/// context (home dir, blueprint name, host platform) and returns either a
/// target path string or rendered content bytes. Errors are flat strings —
/// caller composes them into the user-facing message.
///
/// `std::function` is copyable; both halves can hold their own state
/// (e.g. shared_ptr<LuaRef> for Lua-backed renderers). Copying a
/// RendererFns extends the lifetime of any captured Lua refs along with
/// it — see lua_frontend.cpp for the shared_ptr<LuaRef> pattern.
struct RendererFns {
    std::function<std::expected<std::string, std::string>(
        const nlohmann::json& cfg,
        const luban::config_renderer::Context& ctx)> target_path;

    std::function<std::expected<std::string, std::string>(
        const nlohmann::json& cfg,
        const luban::config_renderer::Context& ctx)> render;
};

}  // namespace luban::render_types
