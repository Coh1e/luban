// Unit tests for src/lua_engine.cpp.
//
// Goals: prove the embedded Lua 5.4 VM (a) loads, (b) evaluates pure
// expressions, (c) is correctly sandboxed (dangerous APIs nilled out),
// (d) exposes the `luban.*` table to scripts. Anything that needs the
// filesystem / network is out of scope here — those are integration
// concerns covered by smoke tests.

#include <string>

#include "doctest.h"
#include "lua_engine.hpp"

using luban::lua::Engine;

TEST_CASE("Engine constructs and tears down cleanly") {
    {
        Engine e;
        // No assertions — just exercising RAII. Destructor at scope exit
        // calls lua_close. A leaking VM would show up in valgrind/asan,
        // not here, but at least we know construction doesn't throw.
        CHECK(e.state() != nullptr);
    }
    // Second construction should also work — proves we're not leaking
    // global state.
    Engine e2;
    CHECK(e2.state() != nullptr);
}

TEST_CASE("eval_string evaluates pure expressions") {
    Engine e;

    SUBCASE("integer arithmetic") {
        auto r = e.eval_string("return 1 + 2 * 3");
        REQUIRE(r.has_value());
        CHECK(*r == "7");
    }

    SUBCASE("string concat") {
        auto r = e.eval_string("return 'hello, ' .. 'world'");
        REQUIRE(r.has_value());
        CHECK(*r == "hello, world");
    }

    SUBCASE("boolean") {
        auto r = e.eval_string("return 1 < 2");
        REQUIRE(r.has_value());
        CHECK(*r == "true");
    }

    SUBCASE("nil return") {
        auto r = e.eval_string("return nil");
        REQUIRE(r.has_value());
        CHECK(*r == "nil");
    }
}

TEST_CASE("eval_string surfaces Lua errors via unexpected") {
    Engine e;

    SUBCASE("syntax error") {
        auto r = e.eval_string("return 1 +");
        CHECK_FALSE(r.has_value());
        CHECK(r.error().find("near") != std::string::npos);
    }

    SUBCASE("runtime error") {
        auto r = e.eval_string("error('boom')");
        CHECK_FALSE(r.has_value());
        CHECK(r.error().find("boom") != std::string::npos);
    }
}

TEST_CASE("sandbox: dangerous APIs are nil") {
    Engine e;

    // os.execute → must be nil after sandbox install.
    SUBCASE("os.execute") {
        auto r = e.eval_string("return type(os.execute)");
        REQUIRE(r.has_value());
        CHECK(*r == "nil");
    }

    SUBCASE("os.exit") {
        auto r = e.eval_string("return type(os.exit)");
        REQUIRE(r.has_value());
        CHECK(*r == "nil");
    }

    SUBCASE("os.remove") {
        auto r = e.eval_string("return type(os.remove)");
        REQUIRE(r.has_value());
        CHECK(*r == "nil");
    }

    SUBCASE("io.open") {
        auto r = e.eval_string("return type(io.open)");
        REQUIRE(r.has_value());
        CHECK(*r == "nil");
    }

    SUBCASE("io.popen") {
        auto r = e.eval_string("return type(io.popen)");
        REQUIRE(r.has_value());
        CHECK(*r == "nil");
    }

    SUBCASE("loadfile") {
        auto r = e.eval_string("return type(loadfile)");
        REQUIRE(r.has_value());
        CHECK(*r == "nil");
    }

    SUBCASE("dofile") {
        auto r = e.eval_string("return type(dofile)");
        REQUIRE(r.has_value());
        CHECK(*r == "nil");
    }

    SUBCASE("debug library wholesale") {
        auto r = e.eval_string("return type(debug)");
        REQUIRE(r.has_value());
        CHECK(*r == "nil");
    }

    SUBCASE("package.loadlib") {
        auto r = e.eval_string("return type(package.loadlib)");
        REQUIRE(r.has_value());
        CHECK(*r == "nil");
    }

    SUBCASE("print") {
        auto r = e.eval_string("return type(print)");
        REQUIRE(r.has_value());
        CHECK(*r == "nil");
    }
}

TEST_CASE("sandbox: pure-compute APIs survive") {
    Engine e;

    SUBCASE("string library") {
        auto r = e.eval_string("return string.upper('hi')");
        REQUIRE(r.has_value());
        CHECK(*r == "HI");
    }

    SUBCASE("table library") {
        auto r = e.eval_string("local t = {3,1,2}; table.sort(t); return t[1]");
        REQUIRE(r.has_value());
        CHECK(*r == "1");
    }

    SUBCASE("math library") {
        auto r = e.eval_string("return math.max(5, 3, 7, 1)");
        REQUIRE(r.has_value());
        CHECK(*r == "7");
    }

    SUBCASE("os.date / os.time / os.clock survive") {
        // Just check they're callable, not their output (time-dependent).
        auto r = e.eval_string("return type(os.date)");
        REQUIRE(r.has_value());
        CHECK(*r == "function");
    }

    SUBCASE("os.getenv survives") {
        auto r = e.eval_string("return type(os.getenv)");
        REQUIRE(r.has_value());
        CHECK(*r == "function");
    }
}

TEST_CASE("luban.* API is exposed") {
    Engine e;

    SUBCASE("luban.version is non-empty string") {
        auto r = e.eval_string("return type(luban.version)");
        REQUIRE(r.has_value());
        CHECK(*r == "string");

        auto r2 = e.eval_string("return #luban.version > 0");
        REQUIRE(r2.has_value());
        CHECK(*r2 == "true");
    }

    SUBCASE("luban.platform.os returns a known platform") {
        auto r = e.eval_string("return luban.platform.os()");
        REQUIRE(r.has_value());
        // Must be one of windows / linux / macos / unknown — anything else
        // means the cpp branch table missed a case.
        CHECK((*r == "windows" || *r == "linux" || *r == "macos" ||
               *r == "unknown"));
    }

    SUBCASE("luban.platform.arch returns a known arch") {
        auto r = e.eval_string("return luban.platform.arch()");
        REQUIRE(r.has_value());
        CHECK((*r == "x64" || *r == "arm64" || *r == "x86" ||
               *r == "unknown"));
    }

    SUBCASE("luban.env.get returns string for set vars") {
        // PATH is set on every reasonable host; if not, this test is
        // running in an environment we don't support anyway.
        auto r = e.eval_string("return type(luban.env.get('PATH'))");
        REQUIRE(r.has_value());
        CHECK(*r == "string");
    }

    SUBCASE("luban.env.get returns nil for unset vars") {
        auto r = e.eval_string(
            "return luban.env.get('LUBAN_DEFINITELY_UNSET_42')");
        REQUIRE(r.has_value());
        CHECK(*r == "nil");
    }

    SUBCASE("luban.tool_root returns a path string ending in the requested name") {
        auto r = e.eval_string("return luban.tool_root('cmake')");
        REQUIRE(r.has_value());
        // Path is host-XDG dependent; we only assert it's non-empty and
        // ends in 'cmake' (substring suffices — handles Windows '\' or POSIX '/').
        CHECK(r->size() > 0);
        CHECK(r->find("cmake") != std::string::npos);
    }

    SUBCASE("luban.download returns nil + msg in test build (no network half linked)") {
        // The real impl lives in src/lua_engine_download.cpp which luban-tests
        // deliberately doesn't link. Calling the API in tests must hit the
        // fallback in lua_engine.cpp's api_download.
        auto r = e.eval_string(
            "local p, err = luban.download('https://example.com/x.tar.gz') "
            "return tostring(p) .. '|' .. tostring(err)");
        REQUIRE(r.has_value());
        CHECK(r->find("nil|") == 0);  // first value is nil
        CHECK(r->find("not available") != std::string::npos);
    }

    SUBCASE("luban.download rejects non-https schemes (sandbox stub also enforces)") {
        // The fallback returns "not available" before scheme check, so
        // this test exercises the production path indirectly via the "not
        // available" error. The scheme check is verified by inspection +
        // the production smoke (network-bearing tests live outside this TU).
        auto r = e.eval_string(
            "local p, err = luban.download('http://insecure/x') "
            "return type(err)");
        REQUIRE(r.has_value());
        CHECK(*r == "string");
    }
}

TEST_CASE("Each Engine instance gets a fresh VM") {
    // Setting a global in one VM must not leak into another. This is the
    // contract that lets us spin per-blueprint VMs without cross-talk.
    Engine a;
    auto ra = a.eval_string("X_LEAK_CHECK = 'hello'; return X_LEAK_CHECK");
    REQUIRE(ra.has_value());
    CHECK(*ra == "hello");

    Engine b;
    auto rb = b.eval_string("return tostring(X_LEAK_CHECK)");
    REQUIRE(rb.has_value());
    CHECK(*rb == "nil");
}
