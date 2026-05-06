// `program_renderer` — translate a [programs.X] config block from a
// blueprint into the bytes the actual tool wants to read on disk.
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
// Module lookup order (first hit wins):
//   1. <config>/luban/programs/<tool>.lua  -- user override
//   2. embedded_programs::<tool>_lua       -- builtin shipped with luban.exe
//
// Both paths run in a fresh sandboxed lua_engine::Engine, so user
// overrides get the same restricted environment + luban.* API as
// everything else.

#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

#include "json.hpp"

namespace luban::program_renderer {

/// Context handed to renderers as the second argument. Contains the
/// per-apply environment: where home is, what the host OS is, the name
/// of the blueprint that owns this [programs.X] block. Renderers
/// compose target paths from these.
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

/// Render a [programs.X] block. `tool_name` selects the renderer module
/// (lookup order above). `cfg` is the JSON-shaped config block from the
/// blueprint. Returns RenderResult on success, error string on failure
/// (Lua syntax / runtime error / module shape violation / unknown tool).
[[nodiscard]] std::expected<RenderResult, std::string> render(
    std::string_view tool_name, const nlohmann::json& cfg,
    const Context& ctx);

/// Lower-level entry point: render directly from a Lua source string.
/// Used in tests to verify renderers without going through the file/
/// embedded lookup path.
[[nodiscard]] std::expected<RenderResult, std::string> render_with_source(
    std::string_view lua_source, std::string_view chunk_name,
    const nlohmann::json& cfg, const Context& ctx);

}  // namespace luban::program_renderer
