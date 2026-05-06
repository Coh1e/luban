// Unit tests for src/blueprint_lua.cpp.

#include <string>

#include "blueprint_lua.hpp"
#include "doctest.h"

namespace bp = luban::blueprint;
namespace bpl = luban::blueprint_lua;

TEST_CASE("parse_string accepts a minimal valid blueprint") {
    auto r = bpl::parse_string(R"(
return {
  schema = 1,
  name = "minimal",
  description = "tiny test",
}
)");
    REQUIRE(r.has_value());
    CHECK(r->name == "minimal");
    CHECK(r->description == "tiny test");
    CHECK(r->schema == 1);
}

TEST_CASE("blueprint must be a table") {
    auto r = bpl::parse_string("return 42");
    CHECK_FALSE(r.has_value());
    CHECK(r.error().find("table") != std::string::npos);
}

TEST_CASE("missing required `name` is an error") {
    auto r = bpl::parse_string("return { description = 'no name' }");
    CHECK_FALSE(r.has_value());
    CHECK(r.error().find("name") != std::string::npos);
}

TEST_CASE("Lua syntax error surfaces") {
    auto r = bpl::parse_string("return { x =");
    CHECK_FALSE(r.has_value());
    CHECK(r.error().find("Lua") != std::string::npos);
}

TEST_CASE("Lua runtime error surfaces") {
    auto r = bpl::parse_string("error('boom')");
    CHECK_FALSE(r.has_value());
    CHECK(r.error().find("boom") != std::string::npos);
}

TEST_CASE("[tools] with source shorthand parses") {
    auto r = bpl::parse_string(R"(
return {
  name = "x",
  tools = {
    ripgrep = { source = "github:BurntSushi/ripgrep", version = "14.0.3" },
  },
}
)");
    REQUIRE(r.has_value());
    REQUIRE(r->tools.size() == 1);
    auto& t = r->tools[0];
    CHECK(t.name == "ripgrep");
    CHECK(t.source == "github:BurntSushi/ripgrep");
    CHECK(t.version == "14.0.3");
    CHECK(t.platforms.empty());
}

TEST_CASE("[tools] with inline platforms array parses") {
    auto r = bpl::parse_string(R"(
return {
  name = "x",
  tools = {
    fd = {
      version = "10.2.0",
      platforms = {
        { target = "windows-x64", url = "https://example.com/fd-win.zip",
          sha256 = "deadbeef", bin = "fd.exe" },
        { target = "linux-x64", url = "https://example.com/fd-linux.tar.gz",
          sha256 = "cafef00d", bin = "fd" },
      },
    },
  },
}
)");
    REQUIRE(r.has_value());
    REQUIRE(r->tools.size() == 1);
    auto& t = r->tools[0];
    REQUIRE(t.platforms.size() == 2);
    CHECK(t.platforms[0].target == "windows-x64");
    CHECK(t.platforms[1].target == "linux-x64");
}

TEST_CASE("tool with neither source nor platforms is an error") {
    auto r = bpl::parse_string(R"(
return {
  name = "x",
  tools = { broken = { version = "1.0" } },
}
)");
    CHECK_FALSE(r.has_value());
    CHECK(r.error().find("source") != std::string::npos);
}

TEST_CASE("[programs] becomes a JSON config blob") {
    auto r = bpl::parse_string(R"(
return {
  name = "x",
  programs = {
    git = {
      userName = "Coh1e",
      aliases = { co = "checkout", br = "branch" },
    },
    bat = {
      theme = "ansi",
      style = { "numbers", "changes", "header" },
    },
  },
}
)");
    REQUIRE(r.has_value());
    REQUIRE(r->programs.size() == 2);

    auto* git = r->find_program("git");
    REQUIRE(git);
    CHECK(git->config["userName"].get<std::string>() == "Coh1e");
    CHECK(git->config["aliases"]["co"].get<std::string>() == "checkout");

    auto* bat = r->find_program("bat");
    REQUIRE(bat);
    CHECK(bat->config["theme"].get<std::string>() == "ansi");
    REQUIRE(bat->config["style"].is_array());
    CHECK(bat->config["style"].size() == 3);
    CHECK(bat->config["style"][0].get<std::string>() == "numbers");
}

TEST_CASE("[files] replace + drop-in modes") {
    auto r = bpl::parse_string(R"(
return {
  name = "x",
  files = {
    ["~/.config/some-tool/config"] = {
      content = "key=value\n",
      mode = "replace",
    },
    ["~/.gitconfig.d/cli"] = {
      content = "[alias]\n    co = checkout\n",
      mode = "drop-in",
    },
  },
}
)");
    REQUIRE(r.has_value());
    REQUIRE(r->files.size() == 2);
    bool saw_replace = false, saw_dropin = false;
    for (auto& f : r->files) {
        if (f.mode == bp::FileMode::Replace) saw_replace = true;
        if (f.mode == bp::FileMode::DropIn)  saw_dropin  = true;
    }
    CHECK(saw_replace);
    CHECK(saw_dropin);
}

TEST_CASE("conditional / computed fields work via Lua") {
    // Use luban.platform.os() to branch — proves the Lua sandbox + API
    // injection actually flows through to blueprint parsing.
    auto r = bpl::parse_string(R"(
local os_name = luban.platform.os()
return {
  name = "computed-" .. os_name,
  description = "platform was " .. os_name,
}
)");
    REQUIRE(r.has_value());
    // os_name will be one of windows/linux/macos/unknown depending on the
    // host running the test. Just verify the prefix is right.
    CHECK(r->name.starts_with("computed-"));
    CHECK(r->description.starts_with("platform was "));
}

TEST_CASE("[meta] requires + conflicts") {
    auto r = bpl::parse_string(R"(
return {
  name = "x",
  meta = {
    requires = { "embedded:cpp-base", "cli-quality" },
    conflicts = { "legacy-tools" },
  },
}
)");
    REQUIRE(r.has_value());
    REQUIRE(r->meta.requires_.size() == 2);
    CHECK(r->meta.requires_[0] == "embedded:cpp-base");
    REQUIRE(r->meta.conflicts.size() == 1);
    CHECK(r->meta.conflicts[0] == "legacy-tools");
}
