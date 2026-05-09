// Unit tests for `luban describe --json` (DESIGN §10 item 7, M8).
//
// `describe_state::build()` is the pure JSON builder behind both the
// `--json` mode and the human-readable text print path. Pinning its
// schema here protects IDE plugins / agents that key on stable field
// names.

#include <filesystem>
#include <fstream>

#include "describe_state.hpp"
#include "doctest.h"
#include "json.hpp"
#include "luban/version.hpp"
#include "paths.hpp"

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

struct Sandbox {
    fs::path root;
    fs::path original_cwd;
    Sandbox() {
        root = fs::temp_directory_path() /
               ("luban-describe-test-" + std::to_string(::time(nullptr)) + "-" +
                std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(root);
        std::error_code ec;
        original_cwd = fs::current_path(ec);
        // Park cwd outside any luban project so the project tier stays out
        // of build() unless a test deliberately opts in.
        fs::current_path(root, ec);
        ::_putenv_s("LUBAN_PREFIX", root.string().c_str());
        ::_putenv_s("USERPROFILE", root.string().c_str());
    }
    ~Sandbox() {
        std::error_code ec;
        fs::current_path(original_cwd, ec);
        fs::remove_all(root, ec);
    }
};

}  // namespace

TEST_CASE("describe_state::build emits the frozen v1 schema marker") {
    Sandbox sb;
    auto j = luban::describe_state::build();
    REQUIRE(j.contains("schema"));
    CHECK(j["schema"] == 1);
}

TEST_CASE("describe_state::build surfaces the cmake-generated luban_version") {
    Sandbox sb;
    auto j = luban::describe_state::build();
    REQUIRE(j.contains("luban_version"));
    CHECK(j["luban_version"] == std::string(luban::kLubanVersion));
    CHECK(j["luban_version"] != "0.1.0");  // guards G7 regression (hardcoded literal)
}

TEST_CASE("describe_state::build maps every paths::all_dirs entry into paths{}") {
    Sandbox sb;
    auto j = luban::describe_state::build();
    REQUIRE(j.contains("paths"));
    REQUIRE(j["paths"].is_object());
    for (auto& [name, p] : luban::paths::all_dirs()) {
        REQUIRE(j["paths"].contains(name));
        CHECK(j["paths"][name] == p.string());
    }
}

TEST_CASE("describe_state::build keeps installed_components as the empty v0 slot") {
    Sandbox sb;
    auto j = luban::describe_state::build();
    // DESIGN §11: the v0.x installed.json registry is gone; describe still
    // emits the field as an empty array for backwards-compatible consumers
    // (IDE plugins keying off the schema 1 shape).
    REQUIRE(j.contains("installed_components"));
    CHECK(j["installed_components"].is_array());
    CHECK(j["installed_components"].empty());
}

TEST_CASE("describe_state::build omits project tier when cwd is not in a luban project") {
    Sandbox sb;
    auto j = luban::describe_state::build();
    CHECK_FALSE(j.contains("project"));
}

TEST_CASE("describe_state::build surfaces project tier when cwd has vcpkg.json") {
    Sandbox sb;
    auto proj = sb.root / "fake-project";
    fs::create_directories(proj);
    {
        std::ofstream f(proj / "vcpkg.json");
        f << R"({"name": "myproj", "version": "1.2.3", "dependencies": ["fmt"]})";
    }
    std::error_code ec;
    fs::current_path(proj, ec);

    auto j = luban::describe_state::build();
    REQUIRE(j.contains("project"));
    CHECK(j["project"]["name"] == "myproj");
    CHECK(j["project"]["version"] == "1.2.3");
    REQUIRE(j["project"]["dependencies"].is_array());
    REQUIRE(j["project"]["dependencies"].size() == 1);
    CHECK(j["project"]["dependencies"][0]["name"] == "fmt");
}

TEST_CASE("describe_state::build's JSON dump is machine-stable (no random keys)") {
    Sandbox sb;
    auto j1 = luban::describe_state::build();
    auto j2 = luban::describe_state::build();
    CHECK(j1.dump() == j2.dump());
}
