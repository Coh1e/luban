// Tests for src/vcpkg_manifest.cpp — vcpkg.json reader/writer used by
// `luban add` / `luban remove` / `luban sync`. vcpkg.json is the project
// dependency single-source-of-truth (CLAUDE.md invariant 4); a regression
// in the parse → mutate → write round-trip silently corrupts a user's
// dep list. These cases pin the parse / format / add / remove / spec-parse
// contract.

#include "doctest.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "vcpkg_manifest.hpp"
#include "file_util.hpp"

namespace fs = std::filesystem;
namespace vm = luban::vcpkg_manifest;

namespace {

fs::path tmp_path(const std::string& tag) {
    return fs::temp_directory_path() /
           ("luban-vm-test-" + tag + "-" +
            std::to_string(std::hash<std::string>{}(tag)));
}

}  // namespace

// ---- parse_pkg_spec --------------------------------------------------------

TEST_CASE("vcpkg_manifest::parse_pkg_spec splits at @") {
    auto [name, ver] = vm::parse_pkg_spec("fmt@10");
    CHECK(name == "fmt");
    CHECK(ver == "10");
}

TEST_CASE("vcpkg_manifest::parse_pkg_spec accepts dotted versions") {
    auto [name, ver] = vm::parse_pkg_spec("spdlog@1.13.0");
    CHECK(name == "spdlog");
    CHECK(ver == "1.13.0");
}

TEST_CASE("vcpkg_manifest::parse_pkg_spec without @ leaves version empty") {
    // No @ → caller will emit a baseline-only dependency (no `version>=`).
    auto [name, ver] = vm::parse_pkg_spec("fmt");
    CHECK(name == "fmt");
    CHECK(ver.empty());
}

// ---- load / save round-trip ------------------------------------------------

TEST_CASE("vcpkg_manifest::load on missing file yields default with fallback name") {
    auto p = tmp_path("load-missing");
    fs::remove(p);
    auto m = vm::load(p, "my-fallback");
    CHECK(m.name == "my-fallback");
    CHECK(m.version == "0.1.0");
    CHECK(m.dependencies.empty());
}

TEST_CASE("vcpkg_manifest::load parses name/version/dependencies") {
    auto p = tmp_path("load-basic");
    luban::file_util::write_text_atomic(p, R"({
        "name": "myapp",
        "version": "1.2.3",
        "dependencies": ["fmt", "spdlog"]
    })");

    auto m = vm::load(p, "ignored-fallback");
    CHECK(m.name == "myapp");
    CHECK(m.version == "1.2.3");
    REQUIRE(m.dependencies.size() == 2);
    CHECK(m.dependencies[0].name == "fmt");
    CHECK(m.dependencies[1].name == "spdlog");
    CHECK_FALSE(m.dependencies[0].version_ge.has_value());

    fs::remove(p);
}

TEST_CASE("vcpkg_manifest::load parses object-form dependency with version>=") {
    // vcpkg's manifest spec allows {"name":"fmt","version>=":"10"} as the
    // verbose form. Round-tripping through luban should preserve that
    // version constraint.
    auto p = tmp_path("load-versioned");
    luban::file_util::write_text_atomic(p, R"({
        "name": "myapp",
        "version": "0.1.0",
        "dependencies": [
            {"name": "fmt", "version>=": "10"},
            {"name": "spdlog", "version>=": "1.13.0"}
        ]
    })");

    auto m = vm::load(p);
    REQUIRE(m.dependencies.size() == 2);
    REQUIRE(m.dependencies[0].version_ge.has_value());
    CHECK(*m.dependencies[0].version_ge == "10");
    REQUIRE(m.dependencies[1].version_ge.has_value());
    CHECK(*m.dependencies[1].version_ge == "1.13.0");

    fs::remove(p);
}

TEST_CASE("vcpkg_manifest::save round-trips through load") {
    // Note: save() normalizes version_ge to X.Y.Z (vcpkg's `version>=`
    // requires three segments), so what comes back from load is the
    // expanded form, not the original input. That's a documented
    // intentional transform — round-trip stability holds *after the
    // first save*.
    auto p = tmp_path("save-roundtrip");
    fs::remove(p);

    vm::Manifest m;
    m.name = "round-trip-app";
    m.version = "2.0.0";
    vm::add(m, "fmt", "10");          // → save normalizes to "10.0.0"
    vm::add(m, "spdlog", "1.13.5");   // already three segments, no change
    vm::add(m, "boost", "");           // no constraint
    vm::save(p, m);

    auto loaded = vm::load(p);
    CHECK(loaded.name == "round-trip-app");
    CHECK(loaded.version == "2.0.0");
    REQUIRE(loaded.dependencies.size() == 3);
    CHECK(loaded.dependencies[0].name == "fmt");
    REQUIRE(loaded.dependencies[0].version_ge.has_value());
    CHECK(*loaded.dependencies[0].version_ge == "10.0.0");  // normalized
    CHECK(loaded.dependencies[1].name == "spdlog");
    REQUIRE(loaded.dependencies[1].version_ge.has_value());
    CHECK(*loaded.dependencies[1].version_ge == "1.13.5");
    CHECK(loaded.dependencies[2].name == "boost");
    CHECK_FALSE(loaded.dependencies[2].version_ge.has_value());

    fs::remove(p);
}

TEST_CASE("vcpkg_manifest::save normalizes 1- and 2-segment version_ge to X.Y.Z") {
    // vcpkg's `version>=` expects exactly three segments. luban accepts
    // shorthand from users (`luban add fmt@10`) and expands on the way out.
    auto p = tmp_path("save-normalize");
    fs::remove(p);

    vm::Manifest m;
    vm::add(m, "fmt",    "10");      // 1 segment
    vm::add(m, "spdlog", "1.13");    // 2 segments
    vm::add(m, "boost",  "1.83.0");  // already 3 segments
    vm::save(p, m);

    auto loaded = vm::load(p);
    REQUIRE(loaded.dependencies.size() == 3);
    CHECK(*loaded.dependencies[0].version_ge == "10.0.0");
    CHECK(*loaded.dependencies[1].version_ge == "1.13.0");
    CHECK(*loaded.dependencies[2].version_ge == "1.83.0");

    fs::remove(p);
}

TEST_CASE("vcpkg_manifest::save preserves unknown top-level fields") {
    // The Manifest struct only has name/version/dependencies; other
    // top-level keys (features / overrides / supports / builtin-baseline)
    // are kept in `extras` and re-emitted on save. A `luban add` must NOT
    // strip a hand-edited `builtin-baseline`.
    auto p = tmp_path("save-extras");
    luban::file_util::write_text_atomic(p, R"({
        "name": "myapp",
        "version": "0.1.0",
        "dependencies": ["fmt"],
        "builtin-baseline": "abc123def456",
        "overrides": [{"name": "boost", "version": "1.83.0"}]
    })");

    auto m = vm::load(p);
    vm::add(m, "spdlog", "1.13");
    vm::save(p, m);

    // Re-read raw and check the preserved fields are still present.
    std::string text = luban::file_util::read_text(p);
    CHECK(text.find("builtin-baseline") != std::string::npos);
    CHECK(text.find("abc123def456") != std::string::npos);
    CHECK(text.find("overrides") != std::string::npos);
    CHECK(text.find("1.83.0") != std::string::npos);
    // And the new dep landed.
    CHECK(text.find("spdlog") != std::string::npos);

    fs::remove(p);
}

// ---- add (idempotent, replace-on-duplicate) --------------------------------

TEST_CASE("vcpkg_manifest::add with empty version produces a bare-name dep") {
    vm::Manifest m;
    vm::add(m, "fmt", "");
    REQUIRE(m.dependencies.size() == 1);
    CHECK(m.dependencies[0].name == "fmt");
    CHECK_FALSE(m.dependencies[0].version_ge.has_value());
}

TEST_CASE("vcpkg_manifest::add with version sets version_ge") {
    vm::Manifest m;
    vm::add(m, "fmt", "10");
    REQUIRE(m.dependencies.size() == 1);
    REQUIRE(m.dependencies[0].version_ge.has_value());
    CHECK(*m.dependencies[0].version_ge == "10");
}

TEST_CASE("vcpkg_manifest::add is idempotent — re-adding replaces, not duplicates") {
    // `luban add fmt@10` followed by `luban add fmt@11` should leave one
    // entry, not two. Otherwise vcpkg sees a conflicting constraint and
    // refuses to install.
    vm::Manifest m;
    vm::add(m, "fmt", "10");
    vm::add(m, "fmt", "11");
    REQUIRE(m.dependencies.size() == 1);
    REQUIRE(m.dependencies[0].version_ge.has_value());
    CHECK(*m.dependencies[0].version_ge == "11");  // last write wins
}

TEST_CASE("vcpkg_manifest::add preserves order of unrelated deps") {
    // luban.cmake target generation walks dependencies in order; add()
    // mutating an existing entry must not reshuffle the list.
    vm::Manifest m;
    vm::add(m, "fmt", "10");
    vm::add(m, "spdlog", "1.13");
    vm::add(m, "boost", "1.83");
    vm::add(m, "spdlog", "1.14");  // mutate middle entry

    REQUIRE(m.dependencies.size() == 3);
    CHECK(m.dependencies[0].name == "fmt");
    CHECK(m.dependencies[1].name == "spdlog");
    CHECK(m.dependencies[2].name == "boost");
    CHECK(*m.dependencies[1].version_ge == "1.14");
}

// ---- remove -----------------------------------------------------------------

TEST_CASE("vcpkg_manifest::remove returns true on hit, false on miss") {
    vm::Manifest m;
    vm::add(m, "fmt", "10");
    vm::add(m, "spdlog", "1.13");
    CHECK(vm::remove(m, "fmt"));        // hit
    CHECK_FALSE(vm::remove(m, "fmt"));  // already gone
    CHECK_FALSE(vm::remove(m, "ghost"));  // never existed
    REQUIRE(m.dependencies.size() == 1);
    CHECK(m.dependencies[0].name == "spdlog");
}

TEST_CASE("vcpkg_manifest::remove preserves remaining deps' order") {
    vm::Manifest m;
    vm::add(m, "fmt", "10");
    vm::add(m, "spdlog", "1.13");
    vm::add(m, "boost", "1.83");
    REQUIRE(vm::remove(m, "spdlog"));
    REQUIRE(m.dependencies.size() == 2);
    CHECK(m.dependencies[0].name == "fmt");
    CHECK(m.dependencies[1].name == "boost");
}

// ---- BOM tolerance ---------------------------------------------------------

TEST_CASE("vcpkg_manifest::load survives a UTF-8 BOM at file start") {
    // VS Code / Notepad save with UTF-8 BOM by default. nlohmann::json
    // rejects raw BOM, so vcpkg_manifest must strip it before parsing
    // (delegated to file_util::read_text_no_bom under the hood).
    auto p = tmp_path("load-bom");
    {
        std::ofstream out(p, std::ios::binary);
        out.put(static_cast<char>(0xEF));
        out.put(static_cast<char>(0xBB));
        out.put(static_cast<char>(0xBF));
        out << R"({"name":"with-bom","version":"0.1.0","dependencies":["fmt"]})";
    }
    auto m = vm::load(p);
    CHECK(m.name == "with-bom");
    REQUIRE(m.dependencies.size() == 1);
    CHECK(m.dependencies[0].name == "fmt");
    fs::remove(p);
}
