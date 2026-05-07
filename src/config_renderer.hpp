// `config_renderer` — translate a [config.X] block from a blueprint
// into the bytes the actual tool wants to read on disk.
//
// Per DESIGN.md §11.3, this is the home-manager-equivalent layer: each
// tool we want to support has a tiny Lua module declaring two functions:
//
//     local M = {}
//     function M.target_path(cfg, ctx) return ... end
//     function M.render(cfg, ctx)      return ... end
//     return M
//
// The renderer takes opaque cfg (whatever shape the blueprint author
// wrote) and returns (target_path, content). luban then writes that
// content to that path via file_deploy.
//
// Post-AH/AI (DESIGN §24.1): config_renderer is pure callback dispatch
// over a `RendererRegistry`. The 5 builtin renderers and any user
// override at <config>/luban/configs/<X>.lua are pre-loaded into the
// per-apply registry by commands/blueprint.cpp at run_apply start
// (via lua_frontend::wrap_embedded_module). Bp-registered renderers
// (luban.register_renderer in a Lua bp) live in the same registry.
// Single dispatch path, last-wins shadowing.

#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

#include "json.hpp"

namespace luban::renderer_registry { class RendererRegistry; }

namespace luban::config_renderer {

/// Context handed to renderers as the second argument. Contains the
/// per-apply environment: where home is, what the host OS is, the name
/// of the blueprint that owns this [config.X] block. Renderers compose
/// target paths from these.
struct Context {
    std::filesystem::path home;            ///< paths::home() for ~ expansion.
    std::filesystem::path xdg_config;      ///< Typically ~/.config on POSIX,
                                           ///< %APPDATA% on Windows.
    std::string blueprint_name;            ///< For drop-in filename suffix.
    std::string platform;                  ///< "windows" / "linux" / "macos".
};

/// Output of a successful render.
struct RenderResult {
    std::filesystem::path target_path;     ///< Where to write `content`.
    std::string content;                   ///< Bytes to write verbatim.
};

/// Render a [config.X] block via the apply registry. `tool_name` selects
/// the renderer (bp-registered name first, then the pre-loaded builtin
/// for that tool, then user override). Pure callback dispatch — this
/// function does not touch lua C API directly (DESIGN §24.1 AH).
[[nodiscard]] std::expected<RenderResult, std::string> render_with_registry(
    const luban::renderer_registry::RendererRegistry& registry,
    std::string_view tool_name, const nlohmann::json& cfg,
    const Context& ctx);

}  // namespace luban::config_renderer
