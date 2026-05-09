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
#include "lua_frontend.hpp"
#include "render_types.hpp"
#include "renderer_registry.hpp"
#include "resolver_registry.hpp"
#include "resolver_types.hpp"
#include "source_resolver.hpp"

using luban::lua::Engine;
namespace rr = luban::renderer_registry;
namespace sr = luban::resolver_registry;
namespace cr = luban::config_renderer;

// ---- Registry primitives -----------------------------------------------
//
// Post-AH (DESIGN §24.1): the registry stores std::function callbacks,
// not Lua refs. These primitive tests use pure C++ lambdas — proving
// the registry has zero Lua coupling and can be driven by any frontend
// (Lua via lua_frontend, native plugins, tests, etc.).

TEST_CASE("RendererRegistry: register_native, find_native, last-wins") {
    rr::RendererRegistry reg;

    auto make_fns = [](std::string tag) {
        luban::render_types::RendererFns fns;
        fns.target_path = [tag](const nlohmann::json&, const cr::Context&) {
            return std::expected<std::string, std::string>{"path:" + tag};
        };
        fns.render = [tag](const nlohmann::json&, const cr::Context&) {
            return std::expected<std::string, std::string>{"content:" + tag};
        };
        return fns;
    };

    reg.register_native("starship", make_fns("v1"));

    auto* found = reg.find_native("starship");
    REQUIRE(found != nullptr);
    cr::Context ctx;
    nlohmann::json cfg = nlohmann::json::object();
    auto tp = found->target_path(cfg, ctx);
    REQUIRE(tp.has_value());
    CHECK(*tp == "path:v1");
    auto content = found->render(cfg, ctx);
    REQUIRE(content.has_value());
    CHECK(*content == "content:v1");

    // Unknown name → nullptr.
    CHECK(reg.find_native("nonexistent") == nullptr);

    // Last-wins: re-register replaces fns. The old captures (if Lua-backed)
    // would have their shared_ptr<LuaRef> released here; with C++ lambdas
    // we just verify the new fns are reachable.
    reg.register_native("starship", make_fns("v2"));
    auto* refound = reg.find_native("starship");
    REQUIRE(refound != nullptr);
    auto tp2 = refound->target_path(cfg, ctx);
    REQUIRE(tp2.has_value());
    CHECK(*tp2 == "path:v2");
}

TEST_CASE("ResolverRegistry: register_native, find_native, last-wins") {
    sr::ResolverRegistry reg;

    auto make_fn = [](std::string url_tag) {
        return luban::resolver_types::ResolverFn{
            [url_tag](const luban::blueprint::ToolSpec& spec)
                -> std::expected<luban::blueprint_lock::LockedPlatform, std::string> {
                luban::blueprint_lock::LockedPlatform plat;
                plat.url = "https://example.test/" + url_tag + "/" + spec.name;
                plat.sha256 = "deadbeef";
                return plat;
            }};
    };

    reg.register_native("emsdk", make_fn("first"));

    auto* found = reg.find_native("emsdk");
    REQUIRE(found != nullptr);
    luban::blueprint::ToolSpec spec;
    spec.name = "tool42";
    auto plat = (*found)(spec);
    REQUIRE(plat.has_value());
    CHECK(plat->url == "https://example.test/first/tool42");

    CHECK(reg.find_native("nope") == nullptr);

    reg.register_native("emsdk", make_fn("second"));
    auto* refound = reg.find_native("emsdk");
    REQUIRE(refound != nullptr);
    auto plat2 = (*refound)(spec);
    REQUIRE(plat2.has_value());
    CHECK(plat2->url == "https://example.test/second/tool42");
}

TEST_CASE("RendererRegistry: dtor releases Lua-backed refs via shared_ptr") {
    // Validate the AH boundary: a Lua-backed RendererFns wrapped via
    // lua_frontend gets its refs released when the registry destructs.
    // We can't directly observe luaL_unref, but we can verify the engine
    // survives the registry's dtor (no UB / no crash from dangling refs).
    Engine e;
    {
        rr::RendererRegistry reg;
        lua_State* L = e.state();

        // Push two trivial functions and ref them (mimics what
        // api_register_renderer does after validating the module table).
        luaL_dostring(L, "return function() return \"path/x\" end");
        int tp_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        luaL_dostring(L, "return function() return \"content-x\" end");
        int r_ref = luaL_ref(L, LUA_REGISTRYINDEX);

        reg.register_native("foo",
            luban::lua_frontend::wrap_renderer_module(
                L, tp_ref, r_ref, luban::render_types::Capability{}));

        auto* fns = reg.find_native("foo");
        REQUIRE(fns != nullptr);
        cr::Context ctx;
        nlohmann::json cfg = nlohmann::json::object();
        auto tp = fns->target_path(cfg, ctx);
        REQUIRE(tp.has_value());
        CHECK(*tp == "path/x");
    }
    // reg destructed → the std::function copies dropped → shared_ptr<LuaRef>
    // refcounts hit zero → luaL_unref ran on each ref. Engine remains usable.
    CHECK(e.state() != nullptr);
    luaL_dostring(e.state(), "return 1");  // sanity: engine still works
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

TEST_CASE("render_with_registry: unknown name returns clear error") {
    // Phase 6 (DESIGN §24.1 AI) collapsed dispatch to the registry alone.
    // An empty registry no longer falls through to a per-call embedded
    // path — there's only one path. commands/blueprint.cpp pre-loads the
    // 5 builtins into every apply registry; tests that want builtin
    // dispatch should mirror that (see test_config_renderer.cpp's
    // BuiltinKit fixture).
    rr::RendererRegistry reg;

    cr::Context ctx;
    ctx.home = "/h";
    ctx.xdg_config = "/h/.config";
    ctx.blueprint_name = "test-bp";
    ctx.platform = "windows";
    nlohmann::json cfg = nlohmann::json::object();

    auto rendered = cr::render_with_registry(reg, "git", cfg, ctx);
    CHECK_FALSE(rendered.has_value());
    CHECK(rendered.error().find("no renderer for `git`") != std::string::npos);
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
