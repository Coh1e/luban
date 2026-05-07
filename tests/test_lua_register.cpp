// Unit + integration tests for luban.register_renderer / register_resolver
// (Tier 1, DESIGN §9.9, v0.4.0 + v0.4.2).
//
// Two layers:
//
//  1. Registry primitives — register / find / dtor unrefs. Cheap, doesn't
//     exercise any Lua code path beyond what's needed to populate refs.
//
//  2. End-to-end via Engine: a Lua chunk calls register_renderer + uses
//     it via render_with_registry; a separate chunk does the same for
//     register_resolver via resolve_with_registry. Asserts the bp's
//     functions actually fire and produce the expected (path, content)
//     / LockedTool result.
//
// What we DON'T cover here: full bp apply pipeline (commands/blueprint.cpp
// orchestration). That's smoke-test territory — needs network for fetch.

#include <string>
#include <string_view>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include "blueprint.hpp"
#include "config_renderer.hpp"
#include "doctest.h"
#include "lua_engine.hpp"
#include "renderer_registry.hpp"
#include "resolver_registry.hpp"
#include "source_resolver.hpp"

using luban::lua::Engine;
namespace rr = luban::renderer_registry;
namespace sr = luban::resolver_registry;
namespace cr = luban::config_renderer;

// ---- Registry primitives -----------------------------------------------

TEST_CASE("RendererRegistry: register, find, dtor cleans up refs") {
    Engine e;
    {
        rr::RendererRegistry reg;
        e.attach_registry(&reg);

        // Manually deposit two refs so we don't need a Lua test fixture.
        // (The api_register_renderer C function takes the same path; it's
        // the one tested by the end-to-end case below.)
        lua_State* L = e.state();
        lua_pushinteger(L, 42);
        int ref_a = luaL_ref(L, LUA_REGISTRYINDEX);
        lua_pushinteger(L, 99);
        int ref_b = luaL_ref(L, LUA_REGISTRYINDEX);
        reg.register_lua("starship", L, ref_a, ref_b);

        auto found = reg.find("starship");
        REQUIRE(found.has_value());
        CHECK(found->L == L);
        CHECK(found->target_path_ref == ref_a);
        CHECK(found->render_ref == ref_b);

        // Unknown name → nullopt.
        CHECK_FALSE(reg.find("nonexistent").has_value());

        // Last-wins: re-register replaces refs (and unrefs the old ones,
        // which is what dtor cleanup verifies indirectly — no asan complain).
        lua_pushinteger(L, 1);
        int ref_c = luaL_ref(L, LUA_REGISTRYINDEX);
        lua_pushinteger(L, 2);
        int ref_d = luaL_ref(L, LUA_REGISTRYINDEX);
        reg.register_lua("starship", L, ref_c, ref_d);
        auto refound = reg.find("starship");
        REQUIRE(refound.has_value());
        CHECK(refound->target_path_ref == ref_c);
        CHECK(refound->render_ref == ref_d);
    }
    // Dtor at scope exit unrefs everything; engine is still healthy.
    CHECK(e.state() != nullptr);
}

TEST_CASE("ResolverRegistry: register, find, dtor cleans up refs") {
    Engine e;
    {
        sr::ResolverRegistry reg;
        e.attach_resolver_registry(&reg);

        lua_State* L = e.state();
        lua_pushinteger(L, 7);
        int ref_a = luaL_ref(L, LUA_REGISTRYINDEX);
        reg.register_lua("emsdk", L, ref_a);

        auto found = reg.find("emsdk");
        REQUIRE(found.has_value());
        CHECK(found->fn_ref == ref_a);
        CHECK_FALSE(reg.find("nope").has_value());
    }
    CHECK(e.state() != nullptr);
}

// ---- End-to-end: register_renderer ---------------------------------------

TEST_CASE("register_renderer: bp registers + render_with_registry invokes") {
    Engine e;
    rr::RendererRegistry reg;
    e.attach_registry(&reg);

    // Run a Lua chunk that registers a renderer. The chunk's return value
    // doesn't matter; the side effect into reg is what we test.
    constexpr const char* code = R"(
        luban.register_renderer("starship", {
            target_path = function(cfg, ctx)
                return ctx.xdg_config .. "/starship.toml"
            end,
            render = function(cfg, ctx)
                return string.format(
                    "add_newline = %s\ncommand_timeout = %d\n",
                    tostring(cfg.add_newline or false),
                    cfg.command_timeout or 1000)
            end,
        })
    )";
    auto rc = e.eval_string(code);
    REQUIRE(rc.has_value());

    // Now invoke via the public dispatch.
    cr::Context ctx;
    ctx.home = "/home/test";
    ctx.xdg_config = "/home/test/.config";
    ctx.blueprint_name = "test-bp";
    ctx.platform = "windows";
    nlohmann::json cfg = {{"add_newline", false}, {"command_timeout", 500}};

    auto rendered = cr::render_with_registry(reg, "starship", cfg, ctx);
    REQUIRE(rendered.has_value());
    CHECK(rendered->target_path == std::filesystem::path("/home/test/.config/starship.toml"));
    CHECK(rendered->content.find("add_newline = false") != std::string::npos);
    CHECK(rendered->content.find("command_timeout = 500") != std::string::npos);
}

TEST_CASE("register_renderer: missing required fields → Lua error") {
    Engine e;
    rr::RendererRegistry reg;
    e.attach_registry(&reg);

    // No `render` field — should error at register-time, not at invocation.
    constexpr const char* code = R"(
        luban.register_renderer("broken", {
            target_path = function() return "/x" end,
            -- render missing
        })
    )";
    auto rc = e.eval_string(code);
    CHECK_FALSE(rc.has_value());
    CHECK(rc.error().find("render") != std::string::npos);
}

TEST_CASE("register_renderer: no-op when no registry attached") {
    Engine e;
    // Deliberately don't attach_registry — register_renderer should silently
    // accept the call so bps can run in any context (tests, programmatic
    // calls, doctor's preflight, etc.).
    constexpr const char* code = R"(
        luban.register_renderer("x", {
            target_path = function() return "/" end,
            render = function() return "" end,
        })
    )";
    auto rc = e.eval_string(code);
    CHECK(rc.has_value());
}

TEST_CASE("render_with_registry: unknown name falls through to embedded path") {
    Engine e;
    rr::RendererRegistry reg;
    e.attach_registry(&reg);

    cr::Context ctx;
    ctx.home = "/h";
    ctx.xdg_config = "/h/.config";
    ctx.blueprint_name = "test-bp";
    ctx.platform = "windows";
    nlohmann::json cfg = nlohmann::json::object();

    // "git" is a builtin renderer; registry is empty so we fall through.
    auto rendered = cr::render_with_registry(reg, "git", cfg, ctx);
    REQUIRE(rendered.has_value());
    CHECK(rendered->target_path.string().find(".gitconfig.d") != std::string::npos);
}

// ---- End-to-end: register_resolver ---------------------------------------

TEST_CASE("register_resolver: bp registers + resolve_with_registry invokes") {
    Engine e;
    sr::ResolverRegistry reg;
    e.attach_resolver_registry(&reg);

    // Lua bp registers a fake "myscheme" resolver that returns a canned
    // {url, sha256, bin}. The C++ side is what we're testing — the
    // resolver's implementation is just enough to assert dispatch.
    constexpr const char* code = R"(
        luban.register_resolver("myscheme", function(spec)
            return {
                url = "https://example.com/" .. spec.name .. ".zip",
                sha256 = "sha256:deadbeef",
                bin = spec.name .. ".exe",
            }
        end)
    )";
    auto rc = e.eval_string(code);
    REQUIRE(rc.has_value());

    luban::blueprint::ToolSpec spec;
    spec.name = "fake-tool";
    spec.source = "myscheme:fake-tool";
    spec.version = "1.0.0";

    auto locked = luban::source_resolver::resolve_with_registry(spec, &reg);
    REQUIRE(locked.has_value());
    REQUIRE(locked->platforms.size() == 1);
    auto& [target, lp] = *locked->platforms.begin();
    CHECK(lp.url == "https://example.com/fake-tool.zip");
    CHECK(lp.sha256 == "sha256:deadbeef");
    CHECK(lp.bin == "fake-tool.exe");
    CHECK_FALSE(lp.artifact_id.empty());  // computed from name+ver+target+sha
}

TEST_CASE("register_resolver: bp can shadow built-in scheme (last wins)") {
    Engine e;
    sr::ResolverRegistry reg;
    e.attach_resolver_registry(&reg);

    // Register a fake "github" resolver — should preempt the C++ one.
    constexpr const char* code = R"(
        luban.register_resolver("github", function(spec)
            return { url = "https://shadow/" .. spec.name, sha256 = "sha256:0", bin = "x" }
        end)
    )";
    REQUIRE(e.eval_string(code).has_value());

    luban::blueprint::ToolSpec spec;
    spec.name = "ripgrep";
    spec.source = "github:BurntSushi/ripgrep";
    spec.version = "14.0.0";

    auto locked = luban::source_resolver::resolve_with_registry(spec, &reg);
    REQUIRE(locked.has_value());
    auto& [_, lp] = *locked->platforms.begin();
    CHECK(lp.url == "https://shadow/ripgrep");  // shadowed, not real github
}

TEST_CASE("register_resolver: missing required fields in result → error") {
    Engine e;
    sr::ResolverRegistry reg;
    e.attach_resolver_registry(&reg);

    // Resolver returns a table missing `url` — should surface a clear error.
    constexpr const char* code = R"(
        luban.register_resolver("partial", function(spec)
            return { sha256 = "sha256:0", bin = "x" }   -- no url
        end)
    )";
    REQUIRE(e.eval_string(code).has_value());

    luban::blueprint::ToolSpec spec;
    spec.name = "partial-tool";
    spec.source = "partial:foo";
    spec.version = "1.0";

    auto locked = luban::source_resolver::resolve_with_registry(spec, &reg);
    CHECK_FALSE(locked.has_value());
    CHECK(locked.error().find("url") != std::string::npos);
}

TEST_CASE("register_resolver: resolver fn that errors surfaces traceback") {
    Engine e;
    sr::ResolverRegistry reg;
    e.attach_resolver_registry(&reg);

    constexpr const char* code = R"(
        luban.register_resolver("crashy", function(spec)
            error("intentional from inside resolver")
        end)
    )";
    REQUIRE(e.eval_string(code).has_value());

    luban::blueprint::ToolSpec spec;
    spec.name = "crashy-tool";
    spec.source = "crashy:foo";
    spec.version = "1.0";

    auto locked = luban::source_resolver::resolve_with_registry(spec, &reg);
    CHECK_FALSE(locked.has_value());
    CHECK(locked.error().find("intentional") != std::string::npos);
}

TEST_CASE("resolve_with_registry: unknown scheme falls through to error") {
    Engine e;
    sr::ResolverRegistry reg;
    e.attach_resolver_registry(&reg);

    luban::blueprint::ToolSpec spec;
    spec.name = "x";
    spec.source = "unknown-scheme:foo";
    spec.version = "1.0";

    // Registry is empty → falls through to C++ dispatch → unknown scheme.
    auto locked = luban::source_resolver::resolve_with_registry(spec, &reg);
    CHECK_FALSE(locked.has_value());
    CHECK(locked.error().find("unknown source scheme") != std::string::npos);
}
