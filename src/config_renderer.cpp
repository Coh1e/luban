// See `config_renderer.hpp`.
//
// Post-AH/AI (DESIGN §24.1): this TU is pure callback dispatch over a
// `RendererRegistry`. It contains zero Lua C API code. Builtin renderers
// live in templates/configs/<X>.lua, are embedded as
// `embedded_configs::<X>_lua` strings, and pre-loaded into the apply's
// registry by commands/blueprint.cpp via lua_frontend::wrap_embedded_module
// at run_apply start. From here on, dispatch is single-path: bp-registered
// renderers and builtin renderers are indistinguishable to this TU
// (DESIGN §9.9 "无双码路径").

#include "config_renderer.hpp"

#include "renderer_registry.hpp"

namespace luban::config_renderer {

namespace fs = std::filesystem;

std::expected<RenderResult, std::string> render_with_registry(
    const luban::renderer_registry::RendererRegistry& registry,
    std::string_view tool_name, const nlohmann::json& cfg,
    const Context& ctx) {
    auto* fns = registry.find_native(tool_name);
    if (!fns) {
        return std::unexpected(
            "no renderer for `" + std::string(tool_name) +
            "` (no <config>/luban/configs/" + std::string(tool_name) +
            ".lua, no builtin, and no bp registered one)");
    }
    auto tp = fns->target_path(cfg, ctx);
    if (!tp) return std::unexpected(tp.error());
    auto content = fns->render(cfg, ctx);
    if (!content) return std::unexpected(content.error());
    return RenderResult{fs::path(*tp), *content};
}

}  // namespace luban::config_renderer
