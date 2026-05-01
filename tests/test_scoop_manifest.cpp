// Tests for src/scoop_manifest.cpp — Scoop manifest parser. This is the
// security boundary between random GitHub manifests and luban's installer
// pipeline: a regression here either silently accepts a manifest with
// `installer.script` (PowerShell would run unchecked), or rejects a
// legitimate manifest variant. The cases below pin both the **safety**
// invariants (UnsafeManifest on dangerous fields) and the **shape**
// permutations Scoop's manifest format permits.
//
// The luban-specific `luban_mirrors` extension is also covered — that
// extension is what enables ghproxy.com fallback URLs in restricted-
// network installs.

#include "doctest.h"

#include "json.hpp"
#include "scoop_manifest.hpp"

using nlohmann::json;
namespace sm = luban::scoop_manifest;

// ---- safety: unsafe fields rejected ---------------------------------------

TEST_CASE("scoop_manifest::parse rejects installer.script (PowerShell escape hatch)") {
    // Scoop manifests can run PowerShell via `installer`/`pre_install`/etc.
    // luban's whole "no UAC, no untrusted code" model depends on refusing
    // these. The throw type is the load-bearing signal — component.cpp
    // distinguishes UnsafeManifest from IncompleteManifest.
    json m = {
        {"version", "1.0"},
        {"url", "https://example.com/x.zip"},
        {"hash", "abc123"},
        {"installer", {{"script", "Write-Host 'pwned'"}}},
    };
    CHECK_THROWS_AS(sm::parse(m, "evil-pkg"), sm::UnsafeManifest);
}

TEST_CASE("scoop_manifest::parse rejects pre_install / post_install / uninstaller / persist / psmodule") {
    // Each of these can execute code or touch user state outside luban's
    // scope. All six denied fields tested explicitly so a future "let's
    // allow X just this once" doesn't slip through.
    for (auto field : {"pre_install", "post_install", "uninstaller", "persist", "psmodule"}) {
        json m = {
            {"version", "1.0"},
            {"url", "https://example.com/x.zip"},
            {"hash", "abc123"},
            {field, json::array({"do bad stuff"})},
        };
        CAPTURE(field);
        CHECK_THROWS_AS(sm::parse(m, "evil-pkg"), sm::UnsafeManifest);
    }
}

TEST_CASE("scoop_manifest::parse rejects denied fields nested inside an arch block too") {
    // The arch-specific override (e.g. architecture.64bit.installer) must
    // also be filtered — otherwise the per-arch escape hatch defeats the
    // top-level guard.
    json m = {
        {"version", "1.0"},
        {"architecture", {
            {"64bit", {
                {"url", "https://example.com/x.zip"},
                {"hash", "abc123"},
                {"installer", {{"script", "evil"}}},
            }},
        }},
    };
    CHECK_THROWS_AS(sm::parse(m, "evil-arch-pkg"), sm::UnsafeManifest);
}

TEST_CASE("scoop_manifest::parse rejects msi / nsis URLs") {
    // .msi runs the Microsoft Installer (UAC + arbitrary scripts). .nsis
    // is the same risk via NSIS. Both require an overlay manifest that
    // re-targets a plain .zip / .7z if the user really needs them.
    for (auto suffix : {".msi", ".nsis"}) {
        json m = {
            {"version", "1.0"},
            {"url", std::string("https://example.com/installer") + suffix},
            {"hash", "abc123"},
        };
        CAPTURE(suffix);
        CHECK_THROWS_AS(sm::parse(m, "msi-pkg"), sm::UnsafeManifest);
    }
}

// ---- url / hash normalization ---------------------------------------------

TEST_CASE("scoop_manifest::parse defaults missing hash algo to sha256") {
    // Bare hex with no algo prefix is a Scoop convention from the early
    // single-hash era. We default to sha256 since that's what the modern
    // ScoopInstaller/Main bucket uses.
    json m = {
        {"version", "1.0"},
        {"url", "https://example.com/x.zip"},
        {"hash", "abc123def456"},
    };
    auto r = sm::parse(m, "pkg");
    CHECK(r.hash_spec == "sha256:abc123def456");
}

TEST_CASE("scoop_manifest::parse preserves explicit non-sha256 algos") {
    json m = {
        {"version", "1.0"},
        {"url", "https://example.com/x.zip"},
        {"hash", "sha512:abcdef"},
    };
    auto r = sm::parse(m, "pkg");
    CHECK(r.hash_spec == "sha512:abcdef");
}

TEST_CASE("scoop_manifest::parse lower-cases the hash spec") {
    // ScoopInstaller manifests sometimes have UPPERCASE hex (especially
    // for sha512). Normalizing to lowercase keeps hash::verify_file's
    // case-sensitive comparison happy without a second pass.
    json m = {
        {"version", "1.0"},
        {"url", "https://example.com/x.zip"},
        {"hash", "SHA256:ABCDEF"},
    };
    auto r = sm::parse(m, "pkg");
    CHECK(r.hash_spec == "sha256:abcdef");
}

TEST_CASE("scoop_manifest::parse strips '#renamed.zip' suffix from url") {
    // Scoop's `url#dest.zip` syntax tells the installer to save the
    // download under a chosen filename. luban downloads-and-extracts in
    // one go so the rename is irrelevant — we ignore it but must not
    // include it in the URL we GET.
    json m = {
        {"version", "1.0"},
        {"url", "https://example.com/x.zip#renamed.zip"},
        {"hash", "abc"},
    };
    auto r = sm::parse(m, "pkg");
    CHECK(r.url == "https://example.com/x.zip");
}

TEST_CASE("scoop_manifest::parse accepts singleton-list url + hash") {
    // Scoop's manifest schema allows url/hash to be either a string or a
    // 1-element array. Both forms must yield the same parse result.
    json m = {
        {"version", "1.0"},
        {"url", json::array({"https://example.com/x.zip"})},
        {"hash", json::array({"abc"})},
    };
    auto r = sm::parse(m, "pkg");
    CHECK(r.url == "https://example.com/x.zip");
    CHECK(r.hash_spec == "sha256:abc");
}

TEST_CASE("scoop_manifest::parse rejects multi-element url list (Incomplete)") {
    // Scoop allows url/hash to be a parallel array (download N files in
    // one install). luban doesn't model that today; throw Incomplete so
    // the user knows to write an overlay reducing it to a single URL.
    json m = {
        {"version", "1.0"},
        {"url", json::array({"https://a.zip", "https://b.zip"})},
        {"hash", json::array({"abc", "def"})},
    };
    CHECK_THROWS_AS(sm::parse(m, "pkg"), sm::IncompleteManifest);
}

TEST_CASE("scoop_manifest::parse rejects empty url / empty hash") {
    json m1 = {{"version", "1.0"}, {"url", ""}, {"hash", "abc"}};
    CHECK_THROWS_AS(sm::parse(m1, "pkg"), sm::IncompleteManifest);

    json m2 = {{"version", "1.0"}, {"url", "https://x.zip"}, {"hash", ""}};
    CHECK_THROWS_AS(sm::parse(m2, "pkg"), sm::IncompleteManifest);
}

// ---- architecture sub-block ------------------------------------------------

TEST_CASE("scoop_manifest::parse picks the matching architecture block") {
    // The `architecture` map keys are Scoop-style (64bit/32bit/arm64);
    // arch_to_scoop_key translates from luban's triple form. The arch
    // block's url/hash should override the top-level one.
    json m = {
        {"version", "1.0"},
        {"url", "https://default-arch/x.zip"},
        {"hash", "default"},
        {"architecture", {
            {"64bit", {
                {"url", "https://x86_64-specific/x.zip"},
                {"hash", "x64hash"},
            }},
            {"arm64", {
                {"url", "https://arm64-specific/x.zip"},
                {"hash", "armhash"},
            }},
        }},
    };
    auto r64 = sm::parse(m, "pkg", "x86_64");
    CHECK(r64.url == "https://x86_64-specific/x.zip");
    CHECK(r64.hash_spec == "sha256:x64hash");

    auto rarm = sm::parse(m, "pkg", "aarch64");
    CHECK(rarm.url == "https://arm64-specific/x.zip");
    CHECK(rarm.hash_spec == "sha256:armhash");
}

TEST_CASE("scoop_manifest::parse falls through to top-level when arch sub-block missing") {
    // Many manifests just have `url`/`hash` at the top level (single-arch
    // install). The arch block's only purpose is to override; absent
    // override means top-level wins.
    json m = {
        {"version", "1.0"},
        {"url", "https://top-level.zip"},
        {"hash", "abc"},
    };
    auto r = sm::parse(m, "pkg", "x86_64");
    CHECK(r.url == "https://top-level.zip");
}

// ---- bin field permutations ------------------------------------------------

TEST_CASE("scoop_manifest::parse: bin as bare string") {
    json m = {
        {"version", "1.0"},
        {"url", "https://x.zip"},
        {"hash", "abc"},
        {"bin", "tools/foo.exe"},
    };
    auto r = sm::parse(m, "pkg");
    REQUIRE(r.bins.size() == 1);
    CHECK(r.bins[0].relative_path == "tools/foo.exe");
    CHECK(r.bins[0].alias == "foo");  // stem
    CHECK(r.bins[0].prefix_args.empty());
}

TEST_CASE("scoop_manifest::parse: bin as list of bare strings") {
    // Multiple bare strings → multiple entries, each aliased by its stem.
    json m = {
        {"version", "1.0"},
        {"url", "https://x.zip"},
        {"hash", "abc"},
        {"bin", json::array({"a/foo.exe", "b/bar.exe"})},
    };
    auto r = sm::parse(m, "pkg");
    REQUIRE(r.bins.size() == 2);
    CHECK(r.bins[0].alias == "foo");
    CHECK(r.bins[1].alias == "bar");
}

TEST_CASE("scoop_manifest::parse: bin as [rel, alias, args] list") {
    // The 3-element form: [relative_path, custom_alias, "arg1 arg2"].
    // The args string is split on whitespace into prefix_args.
    json m = {
        {"version", "1.0"},
        {"url", "https://x.zip"},
        {"hash", "abc"},
        {"bin", json::array({json::array({"foo.exe", "myfoo", "--config foo.cfg"})})},
    };
    auto r = sm::parse(m, "pkg");
    REQUIRE(r.bins.size() == 1);
    CHECK(r.bins[0].relative_path == "foo.exe");
    CHECK(r.bins[0].alias == "myfoo");
    REQUIRE(r.bins[0].prefix_args.size() == 2);
    CHECK(r.bins[0].prefix_args[0] == "--config");
    CHECK(r.bins[0].prefix_args[1] == "foo.cfg");
}

TEST_CASE("scoop_manifest::parse: bin as list of [rel, alias] pairs") {
    json m = {
        {"version", "1.0"},
        {"url", "https://x.zip"},
        {"hash", "abc"},
        {"bin", json::array({
            json::array({"a/foo.exe", "foo"}),
            json::array({"b/bar.exe", "bar"}),
        })},
    };
    auto r = sm::parse(m, "pkg");
    REQUIRE(r.bins.size() == 2);
    CHECK(r.bins[0].relative_path == "a/foo.exe");
    CHECK(r.bins[0].alias == "foo");
    CHECK(r.bins[1].alias == "bar");
}

TEST_CASE("scoop_manifest::parse: bin missing → no bins (not an error)") {
    // Some component manifests are pure bundles (no exe surface, e.g.
    // a header-only library archive). Empty bins is the legitimate
    // state — it must not throw.
    json m = {
        {"version", "1.0"},
        {"url", "https://x.zip"},
        {"hash", "abc"},
    };
    auto r = sm::parse(m, "pkg");
    CHECK(r.bins.empty());
}

// ---- extract_dir / extract_to ----------------------------------------------

TEST_CASE("scoop_manifest::parse: extract_dir as string") {
    json m = {
        {"version", "1.0"},
        {"url", "https://x.zip"},
        {"hash", "abc"},
        {"extract_dir", "inner-folder"},
    };
    auto r = sm::parse(m, "pkg");
    REQUIRE(r.extract_dir.has_value());
    CHECK(*r.extract_dir == "inner-folder");
}

TEST_CASE("scoop_manifest::parse: extract_dir as singleton list takes first") {
    json m = {
        {"version", "1.0"},
        {"url", "https://x.zip"},
        {"hash", "abc"},
        {"extract_dir", json::array({"first-folder", "second-folder"})},
    };
    auto r = sm::parse(m, "pkg");
    REQUIRE(r.extract_dir.has_value());
    CHECK(*r.extract_dir == "first-folder");
}

// ---- depends + env_set + env_add_path -------------------------------------

TEST_CASE("scoop_manifest::parse: depends as bare string is wrapped in single-element list") {
    json m = {
        {"version", "1.0"},
        {"url", "https://x.zip"},
        {"hash", "abc"},
        {"depends", "node"},
    };
    auto r = sm::parse(m, "pkg");
    REQUIRE(r.depends.size() == 1);
    CHECK(r.depends[0] == "node");
}

TEST_CASE("scoop_manifest::parse: depends as list preserved") {
    // emscripten.json depends on ["node"]. setup.cpp::expand_depends does
    // DFS post-order so node installs first.
    json m = {
        {"version", "1.0"},
        {"url", "https://x.zip"},
        {"hash", "abc"},
        {"depends", json::array({"node", "git"})},
    };
    auto r = sm::parse(m, "pkg");
    REQUIRE(r.depends.size() == 2);
    CHECK(r.depends[0] == "node");
    CHECK(r.depends[1] == "git");
}

TEST_CASE("scoop_manifest::parse: env_set and env_add_path coerced") {
    json m = {
        {"version", "1.0"},
        {"url", "https://x.zip"},
        {"hash", "abc"},
        {"env_set", {{"FOO_HOME", "$dir"}, {"BAR_OPT", "1"}}},
        {"env_add_path", json::array({"bin", "tools"})},
    };
    auto r = sm::parse(m, "pkg");
    REQUIRE(r.env_set.size() == 2);
    CHECK(r.env_set["FOO_HOME"] == "$dir");
    CHECK(r.env_set["BAR_OPT"] == "1");
    REQUIRE(r.env_add_path.size() == 2);
    CHECK(r.env_add_path[0] == "bin");
}

// ---- luban-specific: mirror list ------------------------------------------

TEST_CASE("scoop_manifest::parse: luban_mirrors at top-level populates mirrors") {
    // Restricted-network use case: ghproxy.com prefixed URL as fallback
    // when github.com is unreachable. Same hash applies to whichever URL
    // succeeds — only the source path varies.
    json m = {
        {"version", "1.0"},
        {"url", "https://github.com/x/x/releases/download/v1/x.zip"},
        {"hash", "abc"},
        {"luban_mirrors", json::array({
            "https://ghproxy.com/https://github.com/x/x/releases/download/v1/x.zip",
            "https://gh.api.99988866.xyz/https://github.com/x/x/releases/download/v1/x.zip",
        })},
    };
    auto r = sm::parse(m, "pkg");
    REQUIRE(r.mirrors.size() == 2);
    CHECK(r.mirrors[0].find("ghproxy.com") != std::string::npos);
    CHECK(r.mirrors[1].find("99988866") != std::string::npos);
}

TEST_CASE("scoop_manifest::parse: luban_mirrors strips '#renamed' suffix per-entry") {
    // Same suffix-stripping rule as the primary URL (Scoop's `#dest.zip`
    // syntax). Mirrors must produce the same downloadable URLs.
    json m = {
        {"version", "1.0"},
        {"url", "https://primary.zip"},
        {"hash", "abc"},
        {"luban_mirrors", json::array({"https://mirror.com/x.zip#renamed.zip"})},
    };
    auto r = sm::parse(m, "pkg");
    REQUIRE(r.mirrors.size() == 1);
    CHECK(r.mirrors[0] == "https://mirror.com/x.zip");
}

TEST_CASE("scoop_manifest::parse: luban_mirrors absent → empty list") {
    json m = {
        {"version", "1.0"},
        {"url", "https://x.zip"},
        {"hash", "abc"},
    };
    auto r = sm::parse(m, "pkg");
    CHECK(r.mirrors.empty());
}

TEST_CASE("scoop_manifest::parse: luban_mirrors inside arch block honoured") {
    // Per-arch mirrors are a real use case: x86_64 zip on github,
    // arm64 zip on a different mirror. arch-block mirror wins over
    // top-level (consistent with url/hash semantics).
    json m = {
        {"version", "1.0"},
        {"architecture", {
            {"64bit", {
                {"url", "https://x.zip"},
                {"hash", "abc"},
                {"luban_mirrors", json::array({"https://mirror64.zip"})},
            }},
        }},
    };
    auto r = sm::parse(m, "pkg", "x86_64");
    REQUIRE(r.mirrors.size() == 1);
    CHECK(r.mirrors[0] == "https://mirror64.zip");
}

// ---- error: not-a-JSON-object ---------------------------------------------

TEST_CASE("scoop_manifest::parse: non-object input throws Incomplete") {
    json arr = json::array({1, 2, 3});
    CHECK_THROWS_AS(sm::parse(arr, "pkg"), sm::IncompleteManifest);

    json scalar = "just a string";
    CHECK_THROWS_AS(sm::parse(scalar, "pkg"), sm::IncompleteManifest);
}

// ---- version handling ------------------------------------------------------

TEST_CASE("scoop_manifest::parse: missing version → 'unknown'") {
    // Real Scoop manifests always have version, but luban tolerates
    // missing as 'unknown' rather than throwing — the field is metadata,
    // not load-bearing for install pipeline.
    json m = {
        {"url", "https://x.zip"},
        {"hash", "abc"},
    };
    auto r = sm::parse(m, "pkg");
    CHECK(r.version == "unknown");
}

TEST_CASE("scoop_manifest::parse: version trims surrounding whitespace") {
    json m = {
        {"version", "  1.2.3  "},
        {"url", "https://x.zip"},
        {"hash", "abc"},
    };
    auto r = sm::parse(m, "pkg");
    CHECK(r.version == "1.2.3");
}
