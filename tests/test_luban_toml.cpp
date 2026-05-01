// Tests for src/luban_toml.cpp — particularly the cpp ∈ {17,20,23} validation
// added in PR 8 (silently fell-through pre-v0.2; bogus values silently
// became the project standard).

#include "doctest.h"

#include "luban_toml.hpp"

TEST_CASE("luban_toml: empty / missing file yields default Config") {
    auto cfg = luban::luban_toml::load_from_text("");
    CHECK(cfg.project.cpp == 23);
    CHECK(cfg.project.triplet == "x64-mingw-static");
    CHECK(cfg.scaffold.warnings == luban::luban_toml::WarningLevel::Normal);
    CHECK(cfg.scaffold.sanitizers.empty());
}

TEST_CASE("luban_toml: cpp accepts 17 / 20 / 23 (integer form)") {
    for (int v : {17, 20, 23}) {
        auto cfg = luban::luban_toml::load_from_text(
            "[project]\ncpp = " + std::to_string(v) + "\n");
        CHECK(cfg.project.cpp == v);
    }
}

TEST_CASE("luban_toml: cpp accepts 17 / 20 / 23 (string form)") {
    for (const char* v : {"17", "20", "23"}) {
        auto cfg = luban::luban_toml::load_from_text(
            "[project]\ncpp = \"" + std::string(v) + "\"\n");
        CHECK(cfg.project.cpp == std::stoi(v));
    }
}

TEST_CASE("luban_toml: cpp = 11 falls back to 23 with warning (legacy guard)") {
    auto cfg = luban::luban_toml::load_from_text("[project]\ncpp = 11\n");
    CHECK(cfg.project.cpp == 23);
}

TEST_CASE("luban_toml: cpp = 14 falls back to 23 (too old to ship from `luban new`)") {
    auto cfg = luban::luban_toml::load_from_text("[project]\ncpp = 14\n");
    CHECK(cfg.project.cpp == 23);
}

TEST_CASE("luban_toml: cpp = 26 falls back to 23 (compiler not yet universal)") {
    auto cfg = luban::luban_toml::load_from_text("[project]\ncpp = 26\n");
    CHECK(cfg.project.cpp == 23);
}

TEST_CASE("luban_toml: cpp = \"twenty-three\" — unparseable string falls back to 23") {
    auto cfg = luban::luban_toml::load_from_text(
        "[project]\ncpp = \"twenty-three\"\n");
    CHECK(cfg.project.cpp == 23);
}

TEST_CASE("luban_toml: scaffold.sanitizers parses array form") {
    auto cfg = luban::luban_toml::load_from_text(
        "[scaffold]\nsanitizers = [\"address\", \"ub\"]\n");
    REQUIRE(cfg.scaffold.sanitizers.size() == 2);
    CHECK(cfg.scaffold.sanitizers[0] == "address");
    CHECK(cfg.scaffold.sanitizers[1] == "ub");
}

TEST_CASE("luban_toml: warnings = strict / off / normal (else default normal)") {
    using WL = luban::luban_toml::WarningLevel;
    CHECK(luban::luban_toml::load_from_text("[scaffold]\nwarnings = \"off\"\n")
              .scaffold.warnings == WL::Off);
    CHECK(luban::luban_toml::load_from_text("[scaffold]\nwarnings = \"strict\"\n")
              .scaffold.warnings == WL::Strict);
    CHECK(luban::luban_toml::load_from_text("[scaffold]\nwarnings = \"normal\"\n")
              .scaffold.warnings == WL::Normal);
    // Unrecognized → default Normal (silent fallback per design).
    CHECK(luban::luban_toml::load_from_text("[scaffold]\nwarnings = \"loud\"\n")
              .scaffold.warnings == WL::Normal);
}

TEST_CASE("luban_toml: triplet override sticks") {
    auto cfg = luban::luban_toml::load_from_text(
        "[project]\ntriplet = \"x64-windows\"\n");
    CHECK(cfg.project.triplet == "x64-windows");
}
