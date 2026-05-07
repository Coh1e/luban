// Unit + integration tests for src/config_renderer.cpp and the 5
// built-in Lua renderers.
//
// Post-AH/AI (DESIGN §24.1): config_renderer is pure callback dispatch
// over a RendererRegistry. These tests build a registry pre-loaded with
// all 5 builtins via lua_frontend::wrap_embedded_module — the same path
// commands/blueprint.cpp uses in production at apply start. The
// render_with_source tests (custom Lua module shape coverage) load
// inline source via wrap_embedded_module directly.

#include <string>

#include "config_renderer.hpp"
#include "doctest.h"
#include "json.hpp"
#include "lua_engine.hpp"
#include "lua_frontend.hpp"
#include "paths.hpp"
#include "renderer_registry.hpp"

#include "luban/embedded_configs/git.hpp"
#include "luban/embedded_configs/bat.hpp"
#include "luban/embedded_configs/fastfetch.hpp"
#include "luban/embedded_configs/yazi.hpp"
#include "luban/embedded_configs/delta.hpp"

using nlohmann::json;
namespace cr = luban::config_renderer;
namespace rr = luban::renderer_registry;

namespace {

cr::Context default_ctx() {
    cr::Context ctx;
    ctx.home = "/home/test";
    ctx.xdg_config = "/home/test/.config";
    ctx.blueprint_name = "test-bp";
    ctx.platform = "linux";
    return ctx;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

/// Test fixture: an engine + registry pre-loaded with all 5 builtins.
/// Mirrors what commands/blueprint.cpp::preload_builtin_renderers does.
struct BuiltinKit {
    luban::lua::Engine engine;
    rr::RendererRegistry registry;

    BuiltinKit() {
        engine.attach_registry(&registry);
        load_builtin("git",       luban::embedded_configs::git_lua);
        load_builtin("bat",       luban::embedded_configs::bat_lua);
        load_builtin("fastfetch", luban::embedded_configs::fastfetch_lua);
        load_builtin("yazi",      luban::embedded_configs::yazi_lua);
        load_builtin("delta",     luban::embedded_configs::delta_lua);
    }

    void load_builtin(const char* name, const char* source) {
        auto fns = luban::lua_frontend::wrap_embedded_module(
            engine.state(), source, std::string("=embedded:") + name);
        REQUIRE(fns.has_value());
        registry.register_native(name, std::move(*fns));
    }

    auto render(std::string_view name, const json& cfg, const cr::Context& ctx) {
        return cr::render_with_registry(registry, name, cfg, ctx);
    }
};

/// Render an arbitrary Lua module source through wrap_embedded_module +
/// the registry path. Replaces the pre-AH `render_with_source` helper
/// (which spun a fresh per-call engine and is gone with the dual-codepath
/// removal).
std::expected<cr::RenderResult, std::string> render_inline_source(
    std::string_view source, std::string_view chunk, const json& cfg,
    const cr::Context& ctx) {
    luban::lua::Engine engine;
    rr::RendererRegistry reg;
    auto fns = luban::lua_frontend::wrap_embedded_module(
        engine.state(), source, chunk);
    if (!fns) return std::unexpected(fns.error());
    reg.register_native("__inline", std::move(*fns));
    return cr::render_with_registry(reg, "__inline", cfg, ctx);
}

}  // namespace

// ---- dispatch error paths ----------------------------------------------

TEST_CASE("render_with_registry: unknown name returns clear error") {
    rr::RendererRegistry reg;
    auto r = cr::render_with_registry(reg, "luban-no-such-tool-xyz",
                                       json::object(), default_ctx());
    CHECK_FALSE(r.has_value());
    CHECK(contains(r.error(), "no renderer"));
    CHECK(contains(r.error(), "luban-no-such-tool-xyz"));
}

TEST_CASE("wrap_embedded_module: module must return a table") {
    luban::lua::Engine engine;
    auto fns = luban::lua_frontend::wrap_embedded_module(
        engine.state(), "return 42", "=test");
    CHECK_FALSE(fns.has_value());
    CHECK(contains(fns.error(), "must `return"));
}

TEST_CASE("wrap_embedded_module: missing target_path field") {
    luban::lua::Engine engine;
    auto fns = luban::lua_frontend::wrap_embedded_module(
        engine.state(),
        "return { render = function(c, x) return \"x\" end }",
        "=test");
    CHECK_FALSE(fns.has_value());
    CHECK(contains(fns.error(), "target_path"));
}

TEST_CASE("inline render: render returns non-string surfaces error") {
    auto r = render_inline_source(R"(
        return {
            target_path = function(c, x) return "/tmp/x" end,
            render      = function(c, x) return 42 end,
        }
    )", "=test", json::object(), default_ctx());
    CHECK_FALSE(r.has_value());
    CHECK(contains(r.error(), "render"));
    CHECK(contains(r.error(), "string"));
}

TEST_CASE("inline render: minimal viable module works") {
    auto r = render_inline_source(R"(
        return {
            target_path = function(cfg, ctx) return ctx.home .. "/x.conf" end,
            render = function(cfg, ctx) return "name=" .. (cfg.name or "") end,
        }
    )", "=test", json{{"name", "luban"}}, default_ctx());
    REQUIRE(r.has_value());
    CHECK(r->target_path.string() == "/home/test/x.conf");
    CHECK(r->content == "name=luban");
}

// ---- builtin: git ------------------------------------------------------

TEST_CASE("builtin git renderer: standard fields") {
    BuiltinKit kit;
    json cfg = {
        {"userName", "Coh1e"},
        {"userEmail", "x@example.com"},
        {"aliases", {{"co", "checkout"}, {"br", "branch"}}},
        {"core", {{"editor", "vim"}}},
    };
    auto r = kit.render("git", cfg, default_ctx());
    REQUIRE(r.has_value());
    // Drop-in path under ~/.gitconfig.d/ keyed on blueprint name.
    CHECK(r->target_path.string().find(".gitconfig.d") != std::string::npos);
    CHECK(r->target_path.string().find("test-bp.gitconfig") != std::string::npos);

    // Content has [user] block with name/email properly quoted.
    CHECK(contains(r->content, "[user]"));
    CHECK(contains(r->content, "name = \"Coh1e\""));
    CHECK(contains(r->content, "email = \"x@example.com\""));
    // [alias] block.
    CHECK(contains(r->content, "[alias]"));
    CHECK(contains(r->content, "co = \"checkout\""));
    CHECK(contains(r->content, "br = \"branch\""));
    // [core] block.
    CHECK(contains(r->content, "[core]"));
    CHECK(contains(r->content, "editor = \"vim\""));
}

TEST_CASE("builtin git renderer: extra section escape hatch") {
    BuiltinKit kit;
    json cfg = {
        {"userName", "x"},
        {"extra", {{"safe.directory", "/repo/path"}}},
    };
    auto r = kit.render("git", cfg, default_ctx());
    REQUIRE(r.has_value());
    CHECK(contains(r->content, "[safe]"));
    CHECK(contains(r->content, "directory = \"/repo/path\""));
}

// ---- builtin: bat ------------------------------------------------------

TEST_CASE("builtin bat renderer: flag emission") {
    BuiltinKit kit;
    json cfg = {
        {"theme", "ansi"},
        {"style", json::array({"numbers", "changes", "header"})},
        {"paging", "never"},
    };
    auto r = kit.render("bat", cfg, default_ctx());
    REQUIRE(r.has_value());
    CHECK(r->target_path.string() == "/home/test/.config/bat/config");
    CHECK(contains(r->content, "--theme=ansi"));
    CHECK(contains(r->content, "--style=numbers,changes,header"));
    CHECK(contains(r->content, "--paging=never"));
}

TEST_CASE("builtin bat renderer: snake_case → kebab-case flags") {
    BuiltinKit kit;
    json cfg = {
        {"max_columns", 80},
        {"show_all", true},
    };
    auto r = kit.render("bat", cfg, default_ctx());
    REQUIRE(r.has_value());
    CHECK(contains(r->content, "--max-columns=80"));
    CHECK(contains(r->content, "--show-all"));
    // The snake_case form must NOT appear — that would mean the kebab
    // conversion was skipped.
    CHECK_FALSE(contains(r->content, "--max_columns"));
    CHECK_FALSE(contains(r->content, "--show_all"));
}

// ---- builtin: fastfetch -------------------------------------------------

TEST_CASE("builtin fastfetch renderer: produces valid JSON") {
    BuiltinKit kit;
    json cfg = {
        {"modules", json::array({"title", "os", "shell"})},
        {"logo", {{"type", "auto"}}},
    };
    auto r = kit.render("fastfetch", cfg, default_ctx());
    REQUIRE(r.has_value());
    CHECK(r->target_path.string() ==
          "/home/test/.config/fastfetch/config.jsonc");

    // The output should parse as JSON. Round-trip via nlohmann::json
    // and verify the data matches.
    json parsed = json::parse(r->content);
    REQUIRE(parsed.is_object());
    CHECK(parsed["modules"].is_array());
    CHECK(parsed["modules"].size() == 3);
    CHECK(parsed["modules"][0] == "title");
    CHECK(parsed["logo"]["type"] == "auto");
}

// ---- builtin: yazi ------------------------------------------------------

TEST_CASE("builtin yazi renderer: nested [section]") {
    BuiltinKit kit;
    json cfg = {
        {"manager", {{"ratio", json::array({1, 4, 3})}, {"sort_by", "alphabetical"}}},
        {"opener", {{"edit", json::array({"vim", "$@"})}}},
    };
    auto r = kit.render("yazi", cfg, default_ctx());
    REQUIRE(r.has_value());
    CHECK(r->target_path.string() == "/home/test/.config/yazi/yazi.toml");
    CHECK(contains(r->content, "[manager]"));
    CHECK(contains(r->content, "ratio = [1, 4, 3]"));
    CHECK(contains(r->content, "sort_by = \"alphabetical\""));
    CHECK(contains(r->content, "[opener]"));
}

// ---- builtin: delta -----------------------------------------------------

TEST_CASE("builtin delta renderer: wires pager + delta config") {
    BuiltinKit kit;
    json cfg = {
        {"navigate", true},
        {"line-numbers", true},
        {"syntax-theme", "Monokai Extended"},
    };
    auto r = kit.render("delta", cfg, default_ctx());
    REQUIRE(r.has_value());
    // Drop-in path with -delta suffix on blueprint name.
    CHECK(contains(r->target_path.string(), "test-bp-delta.gitconfig"));
    // [core] pager = delta — required for delta to actually run.
    CHECK(contains(r->content, "[core]"));
    CHECK(contains(r->content, "pager = delta"));
    // [interactive] for `git add -p`.
    CHECK(contains(r->content, "[interactive]"));
    CHECK(contains(r->content, "diffFilter"));
    // [delta] block carries the user options.
    CHECK(contains(r->content, "[delta]"));
    CHECK(contains(r->content, "navigate = true"));
    CHECK(contains(r->content, "line-numbers = true"));
    CHECK(contains(r->content, "syntax-theme = \"Monokai Extended\""));
}
