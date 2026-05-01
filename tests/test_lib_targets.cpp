// Tests for src/lib_targets.cpp — the table-driven vcpkg port → cmake
// target lookup that backs `luban add` / `luban sync`.
//
// The table is the public-ish contract: shape of (port, find_package_name,
// targets[]) is what luban_cmake_gen renders into luban.cmake. Catching
// regressions here means catching them before they ship as broken
// luban.cmake outputs.

#include "doctest.h"

#include "lib_targets.hpp"

TEST_CASE("lib_targets::lookup hits a single-target port (fmt)") {
    auto m = luban::lib_targets::lookup("fmt");
    REQUIRE(m.has_value());
    CHECK(m->find_package_name == "fmt");
    REQUIRE(m->link_targets.size() == 1);
    CHECK(m->link_targets[0] == "fmt::fmt");
}

TEST_CASE("lib_targets::lookup hits a multi-target port (abseil)") {
    auto m = luban::lib_targets::lookup("abseil");
    REQUIRE(m.has_value());
    CHECK(m->find_package_name == "absl");
    // abseil exposes several link targets; must include at least one.
    CHECK(m->link_targets.size() > 1);
}

TEST_CASE("lib_targets::lookup misses unknown ports cleanly") {
    auto m = luban::lib_targets::lookup("definitely-not-a-real-port-name-xyzzy");
    CHECK_FALSE(m.has_value());
}

TEST_CASE("lib_targets::lookup is case-sensitive (vcpkg ports are lowercase)") {
    // vcpkg ports are canonically lowercase; lookup follows. "FMT" should miss.
    auto m = luban::lib_targets::lookup("FMT");
    CHECK_FALSE(m.has_value());
}

TEST_CASE("lib_targets: boost-* ports share find_package(Boost) but distinct targets") {
    auto sys = luban::lib_targets::lookup("boost-system");
    auto fs  = luban::lib_targets::lookup("boost-filesystem");
    REQUIRE(sys.has_value());
    REQUIRE(fs.has_value());
    CHECK(sys->find_package_name == "Boost");
    CHECK(fs->find_package_name == "Boost");
    // Different targets — exercising the multi-port-shares-one-package case
    // that the cmake-gen dedup logic relies on.
    CHECK(sys->link_targets[0] != fs->link_targets[0]);
}

TEST_CASE("lib_targets: librdkafka (added in v0.2 PR 10) emits both C and C++ targets") {
    auto m = luban::lib_targets::lookup("librdkafka");
    REQUIRE(m.has_value());
    CHECK(m->find_package_name == "RdKafka");
    REQUIRE(m->link_targets.size() == 2);
    CHECK(m->link_targets[0] == "RdKafka::rdkafka");
    CHECK(m->link_targets[1] == "RdKafka::rdkafka++");
}
