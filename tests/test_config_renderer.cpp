// Unit + integration tests for src/config_renderer.cpp and the 5
// built-in Lua renderers.
//
// These exercise:
//   - dispatch error paths (unknown tool, broken module shape)
//   - a custom-source path (render_with_source) for shape coverage
//   - one realistic config per built-in renderer, verifying both the
//     target_path and the rendered content shape.
//
// Built-in renderer tests don't pin every byte of output (they'd be
// brittle across formatting tweaks); they assert structural properties
// instead — "git renderer's output starts with [user]", "fastfetch
// produces valid JSON", etc.

#include <string>

#include "doctest.h"
#include "json.hpp"
#include "paths.hpp"
#include "config_renderer.hpp"

using nlohmann::json;
namespace pr = luban::config_renderer;

namespace {

pr::Context default_ctx() {
    pr::Context ctx;
    ctx.home = "/home/test";
    ctx.xdg_config = "/home/test/.config";
    ctx.blueprint_name = "test-bp";
    ctx.platform = "linux";
    return ctx;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

// ---- dispatch error paths ----------------------------------------------

TEST_CASE("render(unknown tool) returns clear error") {
    auto r = pr::render("luban-no-such-tool-xyz", json::object(), default_ctx());
    CHECK_FALSE(r.has_value());
    CHECK(contains(r.error(), "no renderer"));
    CHECK(contains(r.error(), "luban-no-such-tool-xyz"));
}

TEST_CASE("render_with_source: module must return a table") {
    auto r = pr::render_with_source("return 42", "=test",
                                    json::object(), default_ctx());
    CHECK_FALSE(r.has_value());
    CHECK(contains(r.error(), "module must"));
}

TEST_CASE("render_with_source: missing target_path field") {
    auto r = pr::render_with_source(R"(
        return { render = function(c, x) return "x" end }
    )", "=test", json::object(), default_ctx());
    CHECK_FALSE(r.has_value());
    CHECK(contains(r.error(), "target_path"));
}

TEST_CASE("render_with_source: render returns non-string") {
    auto r = pr::render_with_source(R"(
        return {
            target_path = function(c, x) return "/tmp/x" end,
            render      = function(c, x) return 42 end,
        }
    )", "=test", json::object(), default_ctx());
    CHECK_FALSE(r.has_value());
    CHECK(contains(r.error(), "render"));
    CHECK(contains(r.error(), "string"));
}

TEST_CASE("render_with_source: minimal viable module works") {
    auto r = pr::render_with_source(R"(
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
    json cfg = {
        {"userName", "Coh1e"},
        {"userEmail", "x@example.com"},
        {"aliases", {{"co", "checkout"}, {"br", "branch"}}},
        {"core", {{"editor", "vim"}}},
    };
    auto r = pr::render("git", cfg, default_ctx());
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
    json cfg = {
        {"userName", "x"},
        {"extra", {{"safe.directory", "/repo/path"}}},
    };
    auto r = pr::render("git", cfg, default_ctx());
    REQUIRE(r.has_value());
    CHECK(contains(r->content, "[safe]"));
    CHECK(contains(r->content, "directory = \"/repo/path\""));
}

// ---- builtin: bat ------------------------------------------------------

TEST_CASE("builtin bat renderer: flag emission") {
    json cfg = {
        {"theme", "ansi"},
        {"style", json::array({"numbers", "changes", "header"})},
        {"paging", "never"},
    };
    auto r = pr::render("bat", cfg, default_ctx());
    REQUIRE(r.has_value());
    CHECK(r->target_path.string() == "/home/test/.config/bat/config");
    CHECK(contains(r->content, "--theme=ansi"));
    CHECK(contains(r->content, "--style=numbers,changes,header"));
    CHECK(contains(r->content, "--paging=never"));
}

TEST_CASE("builtin bat renderer: snake_case → kebab-case flags") {
    json cfg = {
        {"max_columns", 80},
        {"show_all", true},
    };
    auto r = pr::render("bat", cfg, default_ctx());
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
    json cfg = {
        {"modules", json::array({"title", "os", "shell"})},
        {"logo", {{"type", "auto"}}},
    };
    auto r = pr::render("fastfetch", cfg, default_ctx());
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
    json cfg = {
        {"manager", {{"ratio", json::array({1, 4, 3})}, {"sort_by", "alphabetical"}}},
        {"opener", {{"edit", json::array({"vim", "$@"})}}},
    };
    auto r = pr::render("yazi", cfg, default_ctx());
    REQUIRE(r.has_value());
    CHECK(r->target_path.string() == "/home/test/.config/yazi/yazi.toml");
    CHECK(contains(r->content, "[manager]"));
    CHECK(contains(r->content, "ratio = [1, 4, 3]"));
    CHECK(contains(r->content, "sort_by = \"alphabetical\""));
    CHECK(contains(r->content, "[opener]"));
}

// ---- builtin: delta -----------------------------------------------------

TEST_CASE("builtin delta renderer: wires pager + delta config") {
    json cfg = {
        {"navigate", true},
        {"line-numbers", true},
        {"syntax-theme", "Monokai Extended"},
    };
    auto r = pr::render("delta", cfg, default_ctx());
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
