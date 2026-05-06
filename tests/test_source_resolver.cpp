// Unit tests for src/source_resolver.cpp.

#include <string>

#include "blueprint.hpp"
#include "doctest.h"
#include "source_resolver.hpp"

namespace bp = luban::blueprint;
namespace sr = luban::source_resolver;

TEST_CASE("source_scheme parses leading scheme") {
    CHECK(sr::source_scheme("github:BurntSushi/ripgrep") == "github");
    CHECK(sr::source_scheme("path:/some/local/file") == "path");
    CHECK(sr::source_scheme("https://example.com") == "https");

    // No colon → empty scheme.
    CHECK(sr::source_scheme("plain") == "");
    CHECK(sr::source_scheme("") == "");
}

TEST_CASE("source_body parses body after scheme") {
    CHECK(sr::source_body("github:BurntSushi/ripgrep") == "BurntSushi/ripgrep");
    CHECK(sr::source_body("path:/some/file") == "/some/file");
    // No colon → empty body.
    CHECK(sr::source_body("plain") == "");
}

TEST_CASE("inline platforms pass through to LockedTool") {
    bp::ToolSpec spec;
    spec.name = "ripgrep";
    spec.version = "14.0.3";
    spec.source = "github:BurntSushi/ripgrep";  // ignored when inline present

    bp::PlatformSpec p1;
    p1.target = "windows-x64";
    p1.url = "https://example.com/rg-win.zip";
    p1.sha256 = "deadbeef";
    p1.bin = "rg.exe";
    spec.platforms.push_back(p1);

    bp::PlatformSpec p2;
    p2.target = "linux-x64";
    p2.url = "https://example.com/rg-linux.tar.gz";
    p2.sha256 = "cafef00d";
    p2.bin = "rg";
    spec.platforms.push_back(p2);

    auto r = sr::resolve(spec);
    REQUIRE(r.has_value());
    CHECK(r->version == "14.0.3");
    CHECK(r->source == "github:BurntSushi/ripgrep");
    REQUIRE(r->platforms.count("windows-x64") == 1);
    REQUIRE(r->platforms.count("linux-x64") == 1);

    auto& w = r->platforms["windows-x64"];
    CHECK(w.url == "https://example.com/rg-win.zip");
    CHECK(w.sha256 == "deadbeef");
    CHECK(w.bin == "rg.exe");

    auto& l = r->platforms["linux-x64"];
    CHECK(l.url == "https://example.com/rg-linux.tar.gz");
    CHECK(l.sha256 == "cafef00d");
}

TEST_CASE("inline mode wins even when source is also set") {
    // The contract: if user wrote platform blocks, we don't second-guess
    // them by also hitting the network. resolve() returns the inline
    // data as-is.
    bp::ToolSpec spec;
    spec.name = "weird";
    spec.source = "github:nonsense/notreal";
    bp::PlatformSpec p;
    p.target = "windows-x64";
    p.url = "https://example.com/weird.zip";
    p.sha256 = "12345678";
    p.bin = "weird.exe";
    spec.platforms.push_back(p);

    auto r = sr::resolve(spec);
    REQUIRE(r.has_value());
    CHECK(r->platforms.count("windows-x64") == 1);
}

// Note: github: scheme resolution is now implemented (S8) but requires
// network access to api.github.com. We deliberately don't unit-test it
// — see scripts/smoke.bat / nightly CI for the integration coverage
// once it lands. The unit test ensures only that the dispatch routes
// through, by checking that the inline-mode short-circuit still wins
// for tools with both source AND inline platforms (test below).

TEST_CASE("missing source AND no inline platforms is an error") {
    bp::ToolSpec spec;
    spec.name = "naked";
    auto r = sr::resolve(spec);
    CHECK_FALSE(r.has_value());
    CHECK(r.error().find("naked") != std::string::npos);
}

TEST_CASE("unknown scheme is rejected") {
    bp::ToolSpec spec;
    spec.name = "something";
    spec.source = "unknown-scheme:foo";
    auto r = sr::resolve(spec);
    CHECK_FALSE(r.has_value());
    CHECK(r.error().find("unknown") != std::string::npos);
}

TEST_CASE("malformed source (no colon) is rejected") {
    bp::ToolSpec spec;
    spec.name = "broken";
    spec.source = "just-some-string-with-no-colon";
    auto r = sr::resolve(spec);
    CHECK_FALSE(r.has_value());
    CHECK(r.error().find("malformed") != std::string::npos);
}
