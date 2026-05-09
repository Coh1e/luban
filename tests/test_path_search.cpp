// Tests for src/path_search.cpp — the consolidated PATH lookup helper.
// path_search is the only PATH-resolution code path in luban (doctor and
// perception delegate to it), so a regression here would silently break
// `luban doctor`'s "tools on PATH" check and `luban describe --host`'s
// host snapshot. The cases below pin lookup-hit, lookup-miss, and the
// Windows-specific extension iteration.

#include "doctest.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "path_search.hpp"

namespace fs = std::filesystem;

namespace {

// Test fixture: drop a synthetic executable file into a temp dir, splice
// that dir at the front of PATH, run the lookup, restore PATH. The file's
// content doesn't matter — fs::exists is what path_search probes.
struct PathFixture {
    fs::path dir;
    std::string saved_path;

    PathFixture() {
        dir = fs::temp_directory_path() / ("luban-path-test-" +
            std::to_string(std::hash<std::string>{}(
                std::to_string(reinterpret_cast<uintptr_t>(this)))));
        fs::create_directories(dir);
        const char* p = std::getenv("PATH");
        saved_path = p ? p : "";
        std::string augmented = dir.string() + ";" + saved_path;
        _putenv_s("PATH", augmented.c_str());
    }

    ~PathFixture() {
        _putenv_s("PATH", saved_path.c_str());
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    void touch(const std::string& name) {
        std::ofstream(dir / name).put('\0');
    }
};

}  // namespace

TEST_CASE("path_search::on_path returns nullopt for a non-existent tool") {
    PathFixture fix;
    auto p = luban::path_search::on_path("definitely-not-a-tool-zxqv");
    CHECK_FALSE(p.has_value());
}

TEST_CASE("path_search::on_path finds an exact-name file in PATH") {
    PathFixture fix;
    // SearchPathW on Windows requires an extension to match. The empty
    // string is the last fallback, but it's not what real cmd executables
    // look like — use .exe which is the typical hit.
    fix.touch("luban-test-tool.exe");
    auto p = luban::path_search::on_path("luban-test-tool");
    REQUIRE(p.has_value());
    CHECK(p->filename().string().find("luban-test-tool") != std::string::npos);
}

TEST_CASE("path_search::on_path tries .cmd / .bat / .com extensions") {
    // luban shims are `.cmd`. SearchPathW alone (with no PATHEXT awareness
    // for bare names) would skip them; on_path iterates explicit
    // extensions to catch them.
    PathFixture fix;
    fix.touch("luban-shim-test.cmd");
    auto p = luban::path_search::on_path("luban-shim-test");
    REQUIRE(p.has_value());
    CHECK(p->extension().string() == ".cmd");
}

TEST_CASE("path_search::on_path prefers .exe over .cmd when both exist") {
    // Iteration order in path_search.cpp is .exe → .cmd → .bat → .com → "".
    // If both are present, the first match wins (which matches real cmd
    // resolution behaviour).
    PathFixture fix;
    fix.touch("luban-dual-test.exe");
    fix.touch("luban-dual-test.cmd");
    auto p = luban::path_search::on_path("luban-dual-test");
    REQUIRE(p.has_value());
    CHECK(p->extension().string() == ".exe");
}

TEST_CASE("path_search::on_path_str empty string on miss") {
    // Convenience wrapper used by doctor.cpp; mirrors the legacy
    // doctor::which() return shape.
    PathFixture fix;
    auto s = luban::path_search::on_path_str("does-not-exist-zxqv");
    CHECK(s.empty());
}
