// Unit tests for src/qjs_engine.cpp.
//
// Mirrors tests/test_lua_engine.cpp in shape: prove the embedded QuickJS-NG
// VM (a) loads, (b) evaluates pure expressions, (c) exposes only safe globals
// (no Os.exec / Std.loadFile / etc., since we did not link quickjs-libc),
// (d) exposes the `luban.*` global to scripts.
//
// Reminder: in luban, JS is a *second-class* scripting layer (see
// docs/DESIGN.md §10.3). These tests hold qjs_engine to the same contract
// as lua_engine, but the user-facing emphasis is on Lua.

#include <string>

#include "doctest.h"
#include "qjs_engine.hpp"

using luban::qjs::Engine;

TEST_CASE("Engine constructs and tears down cleanly") {
    {
        Engine e;
        CHECK(e.context() != nullptr);
    }
    Engine e2;
    CHECK(e2.context() != nullptr);
}

TEST_CASE("eval_string evaluates pure expressions") {
    Engine e;

    SUBCASE("integer arithmetic") {
        auto r = e.eval_string("1 + 2 * 3");
        REQUIRE(r.has_value());
        CHECK(*r == "7");
    }

    SUBCASE("string concat") {
        auto r = e.eval_string("'hello, ' + 'world'");
        REQUIRE(r.has_value());
        CHECK(*r == "hello, world");
    }

    SUBCASE("boolean") {
        auto r = e.eval_string("1 < 2");
        REQUIRE(r.has_value());
        CHECK(*r == "true");
    }

    SUBCASE("undefined / null come through as their own strings") {
        auto r = e.eval_string("undefined");
        REQUIRE(r.has_value());
        CHECK(*r == "undefined");

        auto r2 = e.eval_string("null");
        REQUIRE(r2.has_value());
        CHECK(*r2 == "null");
    }
}

TEST_CASE("eval_string surfaces JS errors via unexpected") {
    Engine e;

    SUBCASE("syntax error") {
        auto r = e.eval_string("1 +");
        CHECK_FALSE(r.has_value());
        // Don't pin the exact message — QuickJS evolves it. Just verify
        // it mentions something parseable.
        CHECK(r.error().size() > 0);
    }

    SUBCASE("runtime error") {
        auto r = e.eval_string("throw new Error('boom')");
        CHECK_FALSE(r.has_value());
        CHECK(r.error().find("boom") != std::string::npos);
    }
}

TEST_CASE("sandbox: dangerous globals are absent (we never linked quickjs-libc)") {
    Engine e;

    // Os.* / Std.* are provided by quickjs-libc.c, which we deliberately
    // did NOT vendor. A correctly-built engine reports them as undefined.
    SUBCASE("Os global is undefined") {
        auto r = e.eval_string("typeof Os");
        REQUIRE(r.has_value());
        CHECK(*r == "undefined");
    }

    SUBCASE("Std global is undefined") {
        auto r = e.eval_string("typeof Std");
        REQUIRE(r.has_value());
        CHECK(*r == "undefined");
    }

    SUBCASE("require is undefined") {
        auto r = e.eval_string("typeof require");
        REQUIRE(r.has_value());
        CHECK(*r == "undefined");
    }

    SUBCASE("module loaders absent") {
        // Neither CommonJS-style require nor ES-module-style direct loaders
        // are wired up. Until we explicitly add a sandboxed module path,
        // these stay undefined.
        auto r = e.eval_string("typeof process");
        REQUIRE(r.has_value());
        CHECK(*r == "undefined");
    }
}

TEST_CASE("pure-compute ECMAScript globals survive") {
    Engine e;

    SUBCASE("Math") {
        auto r = e.eval_string("Math.max(5, 3, 7, 1)");
        REQUIRE(r.has_value());
        CHECK(*r == "7");
    }

    SUBCASE("JSON") {
        auto r = e.eval_string("JSON.stringify({a: 1, b: [2, 3]})");
        REQUIRE(r.has_value());
        CHECK(*r == "{\"a\":1,\"b\":[2,3]}");
    }

    SUBCASE("Array methods") {
        auto r = e.eval_string("[3, 1, 2].sort()[0]");
        REQUIRE(r.has_value());
        CHECK(*r == "1");
    }

    SUBCASE("String methods") {
        auto r = e.eval_string("'hi'.toUpperCase()");
        REQUIRE(r.has_value());
        CHECK(*r == "HI");
    }

    SUBCASE("Date.now is callable") {
        // Don't pin the value; just verify the API exists and returns a
        // number-coerced-to-string.
        auto r = e.eval_string("typeof Date.now()");
        REQUIRE(r.has_value());
        CHECK(*r == "number");
    }
}

TEST_CASE("luban.* API is exposed") {
    Engine e;

    SUBCASE("luban.version is non-empty string") {
        auto r = e.eval_string("typeof luban.version");
        REQUIRE(r.has_value());
        CHECK(*r == "string");

        auto r2 = e.eval_string("luban.version.length > 0");
        REQUIRE(r2.has_value());
        CHECK(*r2 == "true");
    }

    SUBCASE("luban.platform.os returns a known platform") {
        auto r = e.eval_string("luban.platform.os()");
        REQUIRE(r.has_value());
        CHECK((*r == "windows" || *r == "linux" || *r == "macos" ||
               *r == "unknown"));
    }

    SUBCASE("luban.platform.arch returns a known arch") {
        auto r = e.eval_string("luban.platform.arch()");
        REQUIRE(r.has_value());
        CHECK((*r == "x64" || *r == "arm64" || *r == "x86" ||
               *r == "unknown"));
    }

    SUBCASE("luban.env.get returns string for set vars") {
        auto r = e.eval_string("typeof luban.env.get('PATH')");
        REQUIRE(r.has_value());
        CHECK(*r == "string");
    }

    SUBCASE("luban.env.get returns null for unset vars") {
        auto r = e.eval_string("luban.env.get('LUBAN_DEFINITELY_UNSET_42')");
        REQUIRE(r.has_value());
        CHECK(*r == "null");
    }
}

TEST_CASE("Each Engine instance gets a fresh VM") {
    // Setting a global in one VM must not leak into another.
    Engine a;
    auto ra = a.eval_string("globalThis.X_LEAK = 'hello'; X_LEAK");
    REQUIRE(ra.has_value());
    CHECK(*ra == "hello");

    Engine b;
    auto rb = b.eval_string("typeof X_LEAK");
    REQUIRE(rb.has_value());
    CHECK(*rb == "undefined");
}
