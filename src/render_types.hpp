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
#include <vector>

#include "config_renderer.hpp"
#include "json.hpp"

namespace luban::render_types {

/// Renderer-declared capability metadata (DESIGN §4 Config / §7). Surfaces
/// in the apply-time trust summary and `doctor` output so users know what
/// each renderer can touch before they consent. Optional: an absent
/// capability declaration is treated as "permissive / undeclared" rather
/// than fail-closed — keeps backwards compat with renderers that pre-date
/// the field.
struct Capability {
    /// Coarse-grained list of paths this renderer is allowed to write
    /// (display strings, e.g. "~/.config/bat/", "~/.gitconfig.d/"). Empty
    /// = no declared restriction; trust summary surfaces "(undeclared)".
    std::vector<std::string> writable_dirs;

    /// True when the renderer overwrites the entire target file (vs.
    /// drop-in / append modes). DESIGN §4 requires this be visible in the
    /// trust summary.
    bool overwrite = false;

    /// True when apply must always confirm with the user before invoking
    /// this renderer, even for official sources. Used by renderers whose
    /// effects are highly user-visible (terminal profile, font registration).
    bool needs_confirm = false;

    /// True when this renderer touches Windows Terminal / PowerShell
    /// profile / font registration — high-impact on user shell UX. Trust
    /// summary highlights these in red regardless of source officiality.
    bool touches_profile = false;

    /// True when the renderer-declared capability was explicitly set by
    /// the renderer (vs. defaulted by core because the renderer omitted
    /// the field). Used by the trust summary to differentiate
    /// "(undeclared)" from "(no restrictions, declared)".
    bool declared = false;
};

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

    /// Renderer-declared capability. Default-constructed (declared=false)
    /// when the renderer omitted the field.
    Capability capability;
};

}  // namespace luban::render_types
