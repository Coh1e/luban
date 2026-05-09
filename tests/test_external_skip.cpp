// Unit tests for src/external_skip.cpp.
//
// We can't easily fake "scoop installed git" on a CI box, so probe()
// itself is exercised against tools we know are on every reasonable
// host (cmd.exe on Windows, sh on POSIX). The interesting state
// machine work — read/write of <state>/external.json — gets exercised
// directly with a sandbox.

#include <filesystem>
#include <fstream>
#include <string>

#include "doctest.h"
#include "external_skip.hpp"
#include "paths.hpp"

namespace fs = std::filesystem;
namespace ext = luban::external_skip;

namespace {

struct Sandbox {
    fs::path root;
    Sandbox() {
        root = fs::temp_directory_path() /
               ("luban-extskip-test-" + std::to_string(::time(nullptr)) + "-" +
                std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(root);
        ::_putenv_s("LUBAN_PREFIX", root.string().c_str());
    }
    ~Sandbox() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

}  // namespace

TEST_CASE("read returns empty registry when file is missing") {
    Sandbox sb;
    auto reg = ext::read();
    CHECK(reg.tools.empty());
    CHECK(reg.schema == 1);
}

TEST_CASE("write + read round-trip") {
    Sandbox sb;
    ext::Registry reg;
    reg.schema = 1;
    reg.tools["git"] = ext::External{"git", "/usr/bin/git"};
    reg.tools["ripgrep"] = ext::External{"ripgrep", "C:\\scoop\\shims\\rg.exe"};

    REQUIRE(ext::write(reg));
    auto loaded = ext::read();
    CHECK(loaded.tools.size() == 2);
    REQUIRE(loaded.tools.count("git") == 1);
    CHECK(loaded.tools["git"].resolved_path == fs::path("/usr/bin/git"));
    REQUIRE(loaded.tools.count("ripgrep") == 1);
    CHECK(loaded.tools["ripgrep"].resolved_path ==
          fs::path("C:\\scoop\\shims\\rg.exe"));
}

TEST_CASE("read tolerates corrupt json (returns empty rather than throw)") {
    Sandbox sb;
    auto path = luban::paths::state_dir() / "external.json";
    fs::create_directories(path.parent_path());
    {
        std::ofstream out(path);
        out << "{this is not json";
    }
    // Should not throw; should return an empty registry.
    auto reg = ext::read();
    CHECK(reg.tools.empty());
}

TEST_CASE("read tolerates partially-valid json (skips bad entries)") {
    Sandbox sb;
    auto path = luban::paths::state_dir() / "external.json";
    fs::create_directories(path.parent_path());
    {
        std::ofstream out(path);
        out << R"({
            "schema": 1,
            "tools": {
                "good":   { "resolved_path": "/bin/good" },
                "broken": "this should be an object",
                "alsogood": { "resolved_path": "/bin/alsogood" }
            }
        })";
    }
    auto reg = ext::read();
    CHECK(reg.tools.size() == 2);
    CHECK(reg.tools.count("good") == 1);
    CHECK(reg.tools.count("alsogood") == 1);
    CHECK(reg.tools.count("broken") == 0);
}

TEST_CASE("probe returns nullopt for nonexistent tool") {
    Sandbox sb;
    auto r = ext::probe("luban-no-such-tool-here-xyz123");
    CHECK_FALSE(r.has_value());
}

TEST_CASE("probe finds cmd.exe (Windows always has this)") {
    Sandbox sb;
    auto r = ext::probe("cmd");
    REQUIRE(r.has_value());
    // Result should be an absolute path that ends in cmd / cmd.exe.
    auto stem = r->resolved_path.stem().string();
    // case-insensitive match — Windows file paths are.
    for (auto& c : stem) c = static_cast<char>(::tolower(c));
    CHECK(stem == "cmd");
}

TEST_CASE("write + read preserves schema field") {
    Sandbox sb;
    ext::Registry reg;
    reg.schema = 1;
    REQUIRE(ext::write(reg));
    auto loaded = ext::read();
    CHECK(loaded.schema == 1);
}
