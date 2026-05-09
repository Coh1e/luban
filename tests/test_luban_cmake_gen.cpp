// Unit tests for src/luban_cmake_gen.cpp (G13).
//
// `render` is the pure function the smoke test exercises end-to-end via
// luban add / remove. Direct unit coverage hardens it against accidental
// drift in:
//   - find_package emission (vcpkg port → cmake target via lib_targets)
//   - [ports.X] user override precedence
//   - LUBAN_TARGETS line shape
//   - read_targets_from_cmake round-trip parser

#include <filesystem>
#include <fstream>
#include <sstream>

#include "doctest.h"
#include "luban_cmake_gen.hpp"
#include "luban_toml.hpp"
#include "vcpkg_manifest.hpp"

namespace fs = std::filesystem;
namespace cgen = luban::luban_cmake_gen;
namespace lt = luban::luban_toml;
namespace vm = luban::vcpkg_manifest;

namespace {

// Quick helper — substring match. Avoids dragging in a regex dep.
bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("render emits LUBAN_TARGETS from input list") {
    cgen::GenInputs in;
    in.targets = {"app", "mycore"};
    auto out = cgen::render(in);
    CHECK(contains(out, "set(LUBAN_TARGETS app mycore)"));
    CHECK(contains(out, "function(luban_apply target)"));
    CHECK(contains(out, "function(luban_register_targets)"));
}

TEST_CASE("render maps fmt to fmt::fmt via lib_targets table") {
    cgen::GenInputs in;
    vm::Dependency dep;
    dep.name = "fmt";
    in.manifest.dependencies.push_back(dep);

    auto out = cgen::render(in);
    CHECK(contains(out, "find_package(fmt CONFIG REQUIRED)"));
    CHECK(contains(out, "fmt::fmt"));
}

TEST_CASE("render falls back to bare find_package when port unknown") {
    cgen::GenInputs in;
    vm::Dependency dep;
    dep.name = "totally-not-in-the-table-xyz";
    in.manifest.dependencies.push_back(dep);

    auto out = cgen::render(in);
    CHECK(contains(out,
        "find_package(totally-not-in-the-table-xyz CONFIG REQUIRED)"));
}

TEST_CASE("render: [ports.X] override beats lib_targets table") {
    cgen::GenInputs in;
    vm::Dependency dep;
    dep.name = "fmt";
    in.manifest.dependencies.push_back(dep);

    lt::PortHint po;
    po.find_package = "FmtPrivate";
    po.targets = {"FmtPrivate::core"};
    in.toml.ports["fmt"] = po;

    auto out = cgen::render(in);
    CHECK(contains(out, "find_package(FmtPrivate CONFIG REQUIRED)"));
    CHECK(contains(out, "FmtPrivate::core"));
    // Built-in mapping must NOT bleed through.
    CHECK_FALSE(contains(out, "fmt::fmt"));
}

TEST_CASE("render dedups find_package across multiple deps that map to the same package") {
    cgen::GenInputs in;
    // Synthesize the boost case: multiple sub-libraries → one Boost.
    lt::PortHint po;
    po.find_package = "Boost";
    po.targets = {"Boost::asio"};
    in.toml.ports["boost-asio"] = po;
    lt::PortHint po2;
    po2.find_package = "Boost";
    po2.targets = {"Boost::beast"};
    in.toml.ports["boost-beast"] = po2;

    vm::Dependency d1; d1.name = "boost-asio";
    vm::Dependency d2; d2.name = "boost-beast";
    in.manifest.dependencies.push_back(d1);
    in.manifest.dependencies.push_back(d2);

    auto out = cgen::render(in);
    // Boost should appear in find_package exactly once.
    size_t first = out.find("find_package(Boost");
    REQUIRE(first != std::string::npos);
    size_t second = out.find("find_package(Boost", first + 1);
    CHECK(second == std::string::npos);
    // Both link targets remain.
    CHECK(contains(out, "Boost::asio"));
    CHECK(contains(out, "Boost::beast"));
}

TEST_CASE("render emits the (no dependencies declared) banner when manifest is empty") {
    cgen::GenInputs in;
    auto out = cgen::render(in);
    CHECK(contains(out, "(no dependencies declared)"));
}

TEST_CASE("read_targets_from_cmake round-trips render output") {
    auto tmp = fs::temp_directory_path() /
               ("luban-cmakegen-rt-" + std::to_string(::time(nullptr)));
    fs::create_directories(tmp);

    cgen::GenInputs in;
    in.targets = {"app", "mycore", "tests"};
    auto content = cgen::render(in);
    {
        std::ofstream f(tmp / "luban.cmake", std::ios::binary);
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    auto parsed = cgen::read_targets_from_cmake(tmp);
    REQUIRE(parsed.size() == 3);
    CHECK(parsed[0] == "app");
    CHECK(parsed[1] == "mycore");
    CHECK(parsed[2] == "tests");

    std::error_code ec;
    fs::remove_all(tmp, ec);
}

TEST_CASE("read_targets_from_cmake returns empty when luban.cmake is missing") {
    auto tmp = fs::temp_directory_path() /
               ("luban-cmakegen-missing-" + std::to_string(::time(nullptr)));
    fs::create_directories(tmp);

    auto parsed = cgen::read_targets_from_cmake(tmp);
    CHECK(parsed.empty());

    std::error_code ec;
    fs::remove_all(tmp, ec);
}

TEST_CASE("render reflects luban.toml [project] cpp version") {
    cgen::GenInputs in;
    in.toml.project.cpp = 20;
    auto out = cgen::render(in);
    CHECK(contains(out, "cxx_std_20"));
}

TEST_CASE("render reflects luban.toml [scaffold] sanitizers") {
    cgen::GenInputs in;
    in.toml.scaffold.sanitizers = {"address", "undefined"};
    auto out = cgen::render(in);
    CHECK(contains(out, "-fsanitize=address,undefined"));
}
