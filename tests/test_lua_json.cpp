// Unit tests for src/lua_json.cpp.

#include <string>

#include "doctest.h"
#include "lua_engine.hpp"
#include "lua_json.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

using nlohmann::json;

namespace {

/// Convenience: push a JSON value, run a Lua expression that consumes
/// the top of the stack as `v`, and return the result of evaluating
/// that expression.
std::string roundtrip_via_lua(const json& v, const std::string& expr) {
    luban::lua::Engine engine;
    lua_State* L = engine.state();
    luban::lua_json::push(L, v);
    lua_setglobal(L, "v");

    std::string code = "return " + expr;
    auto r = engine.eval_string(code);
    REQUIRE(r.has_value());
    return *r;
}

}  // namespace

TEST_CASE("push: scalars round-trip via tostring") {
    CHECK(roundtrip_via_lua(json("hello"), "v") == "hello");
    CHECK(roundtrip_via_lua(json(42), "v") == "42");
    CHECK(roundtrip_via_lua(json(3.14), "v") == "3.14");
    CHECK(roundtrip_via_lua(json(true), "v") == "true");
    CHECK(roundtrip_via_lua(json(false), "v") == "false");
    CHECK(roundtrip_via_lua(json(nullptr), "v") == "nil");
}

TEST_CASE("push: array becomes 1-based Lua table") {
    auto out = roundtrip_via_lua(json::array({"a", "b", "c"}), "v[1]");
    CHECK(out == "a");
    out = roundtrip_via_lua(json::array({"a", "b", "c"}), "#v");
    CHECK(out == "3");
}

TEST_CASE("push: object becomes Lua table with string keys") {
    json obj = {{"name", "luban"}, {"version", 1}};
    CHECK(roundtrip_via_lua(obj, "v.name") == "luban");
    CHECK(roundtrip_via_lua(obj, "v.version") == "1");
}

TEST_CASE("push: nested object/array structure") {
    json nested = {
        {"tools", json::array({"git", "ripgrep"})},
        {"meta", {{"deep", true}, {"level", 2}}},
    };
    CHECK(roundtrip_via_lua(nested, "v.tools[1]") == "git");
    CHECK(roundtrip_via_lua(nested, "v.tools[2]") == "ripgrep");
    CHECK(roundtrip_via_lua(nested, "tostring(v.meta.deep)") == "true");
    CHECK(roundtrip_via_lua(nested, "tostring(v.meta.level)") == "2");
}

TEST_CASE("push: NUL-safe strings") {
    std::string with_nul = "a\0b";
    with_nul.resize(3);
    json v = with_nul;
    auto out = roundtrip_via_lua(v, "#v");
    CHECK(out == "3");  // length includes the embedded NUL
}

namespace {

/// Build a Lua table on the stack via the given expression and pop it
/// back to JSON for inspection.
json roundtrip_lua_to_json(const std::string& expr) {
    luban::lua::Engine engine;
    lua_State* L = engine.state();
    std::string code = "return " + expr;
    if (luaL_dostring(L, code.c_str()) != LUA_OK) {
        FAIL("Lua eval failed: " << lua_tostring(L, -1));
    }
    auto j = luban::lua_json::pop_value(L, lua_gettop(L));
    lua_pop(L, 1);
    return j;
}

}  // namespace

TEST_CASE("pop: scalars") {
    CHECK(roundtrip_lua_to_json("'hi'") == json("hi"));
    CHECK(roundtrip_lua_to_json("42") == json(42));
    CHECK(roundtrip_lua_to_json("3.14") == json(3.14));
    CHECK(roundtrip_lua_to_json("true") == json(true));
    CHECK(roundtrip_lua_to_json("nil") == json(nullptr));
}

TEST_CASE("pop: contiguous integer keys 1..N → array") {
    auto j = roundtrip_lua_to_json("{ 'a', 'b', 'c' }");
    REQUIRE(j.is_array());
    CHECK(j.size() == 3);
    CHECK(j[0] == "a");
    CHECK(j[1] == "b");
    CHECK(j[2] == "c");
}

TEST_CASE("pop: string keys → object") {
    auto j = roundtrip_lua_to_json("{ name = 'luban', version = 1 }");
    REQUIRE(j.is_object());
    CHECK(j["name"] == "luban");
    CHECK(j["version"] == 1);
}

TEST_CASE("pop: gap in integer keys → object") {
    // {[1]='a', [3]='c'} is NOT a valid array (missing index 2). Falls
    // back to object.
    auto j = roundtrip_lua_to_json("{ [1]='a', [3]='c' }");
    REQUIRE(j.is_object());
}

TEST_CASE("pop: nested table") {
    auto j = roundtrip_lua_to_json(R"({
        tools = { 'git', 'ripgrep' },
        meta = { deep = true, level = 2 }
    })");
    REQUIRE(j.is_object());
    REQUIRE(j["tools"].is_array());
    CHECK(j["tools"][0] == "git");
    CHECK(j["meta"]["deep"] == true);
    CHECK(j["meta"]["level"] == 2);
}

TEST_CASE("push then pop: full round-trip preserves shape") {
    json original = {
        {"name", "ripgrep"},
        {"version", "14.0.3"},
        {"platforms", json::array({"windows-x64", "linux-x64"})},
        {"flags", {{"verbose", true}, {"max_count", 100}}},
    };

    luban::lua::Engine engine;
    lua_State* L = engine.state();
    luban::lua_json::push(L, original);
    auto roundtripped = luban::lua_json::pop_value(L, lua_gettop(L));
    lua_pop(L, 1);

    CHECK(roundtripped == original);
}
