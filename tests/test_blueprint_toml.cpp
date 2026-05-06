// Unit tests for src/blueprint_toml.cpp.

#include <string>

#include "blueprint_toml.hpp"
#include "doctest.h"

namespace bp = luban::blueprint;
namespace bpt = luban::blueprint_toml;

TEST_CASE("parse_string accepts a minimal valid blueprint") {
    auto r = bpt::parse_string(R"(
schema = 1
name = "minimal"
description = "tiny test"
)");
    REQUIRE(r.has_value());
    CHECK(r->name == "minimal");
    CHECK(r->description == "tiny test");
    CHECK(r->schema == 1);
    CHECK(r->tools.empty());
    CHECK(r->programs.empty());
    CHECK(r->files.empty());
}

TEST_CASE("missing required `name` is an error") {
    auto r = bpt::parse_string("description = 'x'");
    CHECK_FALSE(r.has_value());
    CHECK(r.error().find("name") != std::string::npos);
}

TEST_CASE("invalid TOML syntax surfaces as TOML syntax error") {
    auto r = bpt::parse_string("name = ");
    CHECK_FALSE(r.has_value());
    CHECK(r.error().find("TOML syntax") != std::string::npos);
}

TEST_CASE("[tools.X] with `source` shorthand parses") {
    auto r = bpt::parse_string(R"(
name = "x"

[tools.ripgrep]
source = "github:BurntSushi/ripgrep"
version = "14.0.3"
)");
    REQUIRE(r.has_value());
    REQUIRE(r->tools.size() == 1);
    auto& t = r->tools[0];
    CHECK(t.name == "ripgrep");
    CHECK(t.source == "github:BurntSushi/ripgrep");
    CHECK(t.version == "14.0.3");
    CHECK(t.platforms.empty());
}

TEST_CASE("[tools.X] with inline [[platform]] parses") {
    auto r = bpt::parse_string(R"(
name = "x"

[tools.fd]
version = "10.2.0"
[[tools.fd.platform]]
target = "windows-x64"
url = "https://example.com/fd-win.zip"
sha256 = "deadbeef"
bin = "fd.exe"
[[tools.fd.platform]]
target = "linux-x64"
url = "https://example.com/fd-linux.tar.gz"
sha256 = "cafef00d"
bin = "fd"
)");
    REQUIRE(r.has_value());
    REQUIRE(r->tools.size() == 1);
    auto& t = r->tools[0];
    CHECK(t.name == "fd");
    CHECK_FALSE(t.source.has_value());
    REQUIRE(t.platforms.size() == 2);
    CHECK(t.platforms[0].target == "windows-x64");
    CHECK(t.platforms[0].url == "https://example.com/fd-win.zip");
    CHECK(t.platforms[0].sha256 == "deadbeef");
    CHECK(t.platforms[0].bin == "fd.exe");
    CHECK(t.platforms[1].target == "linux-x64");
}

TEST_CASE("[tools.X] post_install field parses (script-relative-path)") {
    auto r = bpt::parse_string(R"(
name = "x"

[tools.vcpkg]
source = "github:microsoft/vcpkg"
post_install = "bootstrap-vcpkg.bat"
)");
    REQUIRE(r.has_value());
    REQUIRE(r->tools.size() == 1);
    auto& t = r->tools[0];
    REQUIRE(t.post_install.has_value());
    CHECK(*t.post_install == "bootstrap-vcpkg.bat");
}

TEST_CASE("[tools.X] post_install absent => optional is empty") {
    auto r = bpt::parse_string(R"(
name = "x"

[tools.fmt]
source = "github:fmtlib/fmt"
)");
    REQUIRE(r.has_value());
    REQUIRE(r->tools.size() == 1);
    CHECK_FALSE(r->tools[0].post_install.has_value());
}

TEST_CASE("[tools.X] external_skip overrides probe target name") {
    auto r = bpt::parse_string(R"(
name = "x"

[tools.openssh]
source = "github:PowerShell/Win32-OpenSSH"
external_skip = "ssh.exe"
)");
    REQUIRE(r.has_value());
    REQUIRE(r->tools.size() == 1);
    auto& t = r->tools[0];
    REQUIRE(t.external_skip.has_value());
    CHECK(*t.external_skip == "ssh.exe");
}

TEST_CASE("[tools.X] external_skip absent => optional is empty") {
    auto r = bpt::parse_string(R"(
name = "x"
[tools.cmake]
source = "github:Kitware/CMake"
)");
    REQUIRE(r.has_value());
    CHECK_FALSE(r->tools[0].external_skip.has_value());
}

TEST_CASE("[tools.X] shims as array populates list in order") {
    auto r = bpt::parse_string(R"(
name = "x"

[tools.openssh]
source = "github:PowerShell/Win32-OpenSSH"
shims  = ["ssh.exe", "ssh-keygen.exe", "ssh-agent.exe"]
)");
    REQUIRE(r.has_value());
    REQUIRE(r->tools.size() == 1);
    auto& t = r->tools[0];
    REQUIRE(t.shims.size() == 3);
    CHECK(t.shims[0] == "ssh.exe");
    CHECK(t.shims[1] == "ssh-keygen.exe");
    CHECK(t.shims[2] == "ssh-agent.exe");
}

TEST_CASE("[tools.X] shims as bare string is shorthand for one entry") {
    auto r = bpt::parse_string(R"(
name = "x"
[tools.fd]
source = "github:sharkdp/fd"
shims  = "fd.exe"
)");
    REQUIRE(r.has_value());
    REQUIRE(r->tools[0].shims.size() == 1);
    CHECK(r->tools[0].shims[0] == "fd.exe");
}

TEST_CASE("[tools.X] shims absent => empty vector") {
    auto r = bpt::parse_string(R"(
name = "x"
[tools.fmt]
source = "github:fmtlib/fmt"
)");
    REQUIRE(r.has_value());
    CHECK(r->tools[0].shims.empty());
}

TEST_CASE("[tools.X] shims with non-string entry is a schema error") {
    // Caught the openssh footgun: silent-drop on type mismatch would have
    // let `shims = [1, 2]` parse successfully and silently install zero
    // shims. We want a loud error instead.
    auto r = bpt::parse_string(R"(
name = "x"
[tools.broken]
source = "github:x/y"
shims  = ["ok.exe", 42]
)");
    CHECK_FALSE(r.has_value());
    CHECK(r.error().find("shims") != std::string::npos);
}

TEST_CASE("[tools.X] shims of wrong scalar type is a schema error") {
    auto r = bpt::parse_string(R"(
name = "x"
[tools.broken]
source = "github:x/y"
shims  = 42
)");
    CHECK_FALSE(r.has_value());
    CHECK(r.error().find("shims") != std::string::npos);
}

TEST_CASE("tool with neither source nor inline platform is an error") {
    auto r = bpt::parse_string(R"(
name = "x"
[tools.broken]
version = "1.0"
)");
    CHECK_FALSE(r.has_value());
    CHECK(r.error().find("source") != std::string::npos);
}

TEST_CASE("inline platform missing required field is an error") {
    auto r = bpt::parse_string(R"(
name = "x"
[tools.fd]
[[tools.fd.platform]]
target = "windows-x64"
# missing url + sha256
)");
    CHECK_FALSE(r.has_value());
    CHECK(r.error().find("url") != std::string::npos);
}

TEST_CASE("[programs.X] becomes a JSON config blob") {
    auto r = bpt::parse_string(R"(
name = "x"

[programs.git]
userName = "Coh1e"
userEmail = "x@example.com"
[programs.git.aliases]
co = "checkout"
br = "branch"

[programs.bat]
theme = "ansi"
style = ["numbers", "changes", "header"]
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

TEST_CASE("[files.X] replace mode with content") {
    auto r = bpt::parse_string(R"(
name = "x"
[files."~/.config/some-tool/config"]
content = """
key=value
"""
mode = "replace"
)");
    REQUIRE(r.has_value());
    REQUIRE(r->files.size() == 1);
    CHECK(r->files[0].target_path == "~/.config/some-tool/config");
    CHECK(r->files[0].mode == bp::FileMode::Replace);
    CHECK(r->files[0].content.find("key=value") != std::string::npos);
}

TEST_CASE("[files.X] drop-in mode") {
    auto r = bpt::parse_string(R"(
name = "x"
[files."~/.gitconfig.d/cli"]
content = "[alias]\n    co = checkout\n"
mode = "drop-in"
)");
    REQUIRE(r.has_value());
    REQUIRE(r->files.size() == 1);
    CHECK(r->files[0].mode == bp::FileMode::DropIn);
}

TEST_CASE("[files.X] unknown mode is an error") {
    auto r = bpt::parse_string(R"(
name = "x"
[files."~/x"]
content = "x"
mode = "weirdmode"
)");
    CHECK_FALSE(r.has_value());
    CHECK(r.error().find("weirdmode") != std::string::npos);
}

TEST_CASE("[meta] requires + conflicts") {
    auto r = bpt::parse_string(R"(
name = "x"
[meta]
requires = ["embedded:cpp-base", "cli-quality"]
conflicts = ["legacy-tools"]
)");
    REQUIRE(r.has_value());
    REQUIRE(r->meta.requires_.size() == 2);
    CHECK(r->meta.requires_[0] == "embedded:cpp-base");
    CHECK(r->meta.requires_[1] == "cli-quality");
    REQUIRE(r->meta.conflicts.size() == 1);
    CHECK(r->meta.conflicts[0] == "legacy-tools");
}

TEST_CASE("find_tool / find_program lookup") {
    auto r = bpt::parse_string(R"(
name = "x"
[tools.ripgrep]
source = "github:BurntSushi/ripgrep"
[tools.fd]
source = "github:sharkdp/fd"
[programs.git]
userName = "test"
)");
    REQUIRE(r.has_value());
    CHECK(r->find_tool("ripgrep") != nullptr);
    CHECK(r->find_tool("fd") != nullptr);
    CHECK(r->find_tool("unknown") == nullptr);
    CHECK(r->find_program("git") != nullptr);
    CHECK(r->find_program("ripgrep") == nullptr);  // ripgrep is a tool, not a program
}

TEST_CASE("multiple errors accumulate (don't fail on first)") {
    // Two separate violations: missing name, broken tool.
    auto r = bpt::parse_string(R"(
description = "no name field above"
[tools.broken]
version = "1.0"
)");
    CHECK_FALSE(r.has_value());
    // Both errors should appear in the joined message — proves the
    // parser doesn't bail on first error, which is helpful for users
    // editing a fresh blueprint.
    CHECK(r.error().find("name") != std::string::npos);
    CHECK(r.error().find("source") != std::string::npos);
}
