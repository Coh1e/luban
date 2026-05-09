// Unit tests for the DESIGN §8 trust model surfaces (G2 + G1 + G10).
//
// Three layers are exercised:
//   1. `source_registry::is_official_url` — owner allowlist parsing
//   2. `source_registry` round-trip preserves the `official` field, and
//      legacy entries (no `official` field) derive correctly on read
//   3. `blueprint_apply::apply`
//      - default-ApplyOptions (source_official=true) proceeds without prompting
//      - non-official source + dry_run proceeds (no destructive effect)
//      - non-official source + !yes + !dry_run + EOF on stdin aborts

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include "applied_db.hpp"
#include "blueprint.hpp"
#include "blueprint_apply.hpp"
#include "blueprint_lock.hpp"
#include "doctest.h"
#include "paths.hpp"
#include "source_registry.hpp"

namespace fs = std::filesystem;
namespace bp = luban::blueprint;
namespace bpa = luban::blueprint_apply;
namespace bpl = luban::blueprint_lock;
namespace sr = luban::source_registry;

namespace {

// RAII redirector for std::cin's streambuf. Swaps in our own buffer for
// the lifetime of the object — apply()'s `getline(std::cin, ...)` reads
// from that instead of the real terminal, so tests stay non-interactive.
struct CinRedirect {
    std::streambuf* original;
    std::istringstream replacement;
    explicit CinRedirect(std::string text) : replacement(std::move(text)) {
        original = std::cin.rdbuf(replacement.rdbuf());
    }
    ~CinRedirect() { std::cin.rdbuf(original); }
};

struct Sandbox {
    fs::path root;
    Sandbox() {
        root = fs::temp_directory_path() /
               ("luban-trust-" + std::to_string(::time(nullptr)) + "-" +
                std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(root);
#ifdef _WIN32
        ::_putenv_s("LUBAN_PREFIX", root.string().c_str());
        ::_putenv_s("USERPROFILE", root.string().c_str());
#else
        ::setenv("LUBAN_PREFIX", root.string().c_str(), 1);
        ::setenv("HOME", root.string().c_str(), 1);
#endif
    }
    ~Sandbox() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

bp::BlueprintSpec make_one_file_spec(const char* name) {
    bp::BlueprintSpec spec;
    spec.schema = 1;
    spec.name = name;
    bp::FileSpec f;
    f.target_path = std::string("~/") + name + "-target.txt";
    f.content = "trust-model fixture\n";
    f.mode = bp::FileMode::Replace;
    spec.files.push_back(std::move(f));
    return spec;
}

}  // namespace

TEST_CASE("is_official_url accepts Coh1e github URLs only") {
    CHECK(sr::is_official_url("https://github.com/Coh1e/luban-bps"));
    CHECK(sr::is_official_url("https://github.com/Coh1e/another-repo"));

    CHECK_FALSE(sr::is_official_url("https://github.com/someone-else/bps"));
    CHECK_FALSE(sr::is_official_url("https://gitlab.com/Coh1e/luban-bps"));
    CHECK_FALSE(sr::is_official_url("file:///D:/local/repo"));
    CHECK_FALSE(sr::is_official_url("https://github.com/"));
    CHECK_FALSE(sr::is_official_url(""));
}

TEST_CASE("source_registry round-trips official field across read/write") {
    Sandbox sb;
    auto path = luban::paths::bp_sources_registry_path();

    std::vector<sr::SourceEntry> entries;
    {
        sr::SourceEntry e;
        e.name = "official_main";
        e.url = "https://github.com/Coh1e/luban-bps";
        e.ref = "main";
        e.commit = "deadbeef";
        e.added_at = "2026-05-09T00:00:00Z";
        e.official = true;
        entries.push_back(e);
    }
    {
        sr::SourceEntry e;
        e.name = "third_party";
        e.url = "https://github.com/somebody-else/bps";
        e.ref = "main";
        e.commit = "cafef00d";
        e.added_at = "2026-05-09T00:00:01Z";
        e.official = false;
        entries.push_back(e);
    }

    REQUIRE(sr::write_file(path, entries).has_value());
    auto loaded = sr::read_file(path);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->size() == 2);

    auto official = sr::find(*loaded, "official_main");
    REQUIRE(official.has_value());
    CHECK(official->official == true);

    auto third = sr::find(*loaded, "third_party");
    REQUIRE(third.has_value());
    CHECK(third->official == false);
}

TEST_CASE("source_registry derives official from URL when field absent (legacy upgrade)") {
    Sandbox sb;
    auto path = luban::paths::bp_sources_registry_path();
    fs::create_directories(path.parent_path());

    // Hand-write a legacy entry with no `official` field, simulating a
    // pre-G2 registry. read() must fall back on is_official_url.
    {
        std::ofstream f(path);
        f << "[source.legacy_official]\n"
          << "url      = \"https://github.com/Coh1e/luban-bps\"\n"
          << "ref      = \"main\"\n"
          << "commit   = \"abc123\"\n"
          << "added_at = \"2026-05-09T00:00:00Z\"\n"
          << "\n"
          << "[source.legacy_third_party]\n"
          << "url      = \"https://github.com/random/bps\"\n"
          << "ref      = \"main\"\n"
          << "commit   = \"def456\"\n"
          << "added_at = \"2026-05-09T00:00:01Z\"\n";
    }

    auto loaded = sr::read_file(path);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->size() == 2);

    auto a = sr::find(*loaded, "legacy_official");
    REQUIRE(a.has_value());
    CHECK(a->official == true);

    auto b = sr::find(*loaded, "legacy_third_party");
    REQUIRE(b.has_value());
    CHECK(b->official == false);
}

TEST_CASE("apply with default ApplyOptions (source_official=true) proceeds without prompt") {
    Sandbox sb;
    auto spec = make_one_file_spec("default-trust");
    bpl::BlueprintLock lock;
    lock.blueprint_name = spec.name;

    bpa::ApplyOptions opts;
    // Default: source_official=true, yes=false, dry_run=false. Should
    // print the summary and proceed without ever blocking on cin.
    auto r = bpa::apply(spec, lock, opts);
    REQUIRE(r.has_value());
    CHECK(fs::exists(luban::paths::home() / "default-trust-target.txt"));
}

TEST_CASE("apply with non-official source + dry_run proceeds without prompt") {
    Sandbox sb;
    auto spec = make_one_file_spec("nonofficial-dryrun");
    bpl::BlueprintLock lock;
    lock.blueprint_name = spec.name;

    bpa::ApplyOptions opts;
    opts.source_official = false;
    opts.dry_run = true;
    opts.bp_source_name = "third_party";

    auto r = bpa::apply(spec, lock, opts);
    REQUIRE(r.has_value());
    // dry-run never lands the file on disk.
    CHECK_FALSE(fs::exists(luban::paths::home() / "nonofficial-dryrun-target.txt"));
}

TEST_CASE("apply with non-official source + --yes proceeds and lands the file") {
    Sandbox sb;
    auto spec = make_one_file_spec("nonofficial-yes");
    bpl::BlueprintLock lock;
    lock.blueprint_name = spec.name;

    bpa::ApplyOptions opts;
    opts.source_official = false;
    opts.yes = true;
    opts.bp_source_name = "third_party";

    auto r = bpa::apply(spec, lock, opts);
    REQUIRE(r.has_value());
    CHECK(fs::exists(luban::paths::home() / "nonofficial-yes-target.txt"));
}

TEST_CASE("apply with non-official source + interactive + EOF aborts") {
    Sandbox sb;
    auto spec = make_one_file_spec("nonofficial-eof");
    bpl::BlueprintLock lock;
    lock.blueprint_name = spec.name;

    bpa::ApplyOptions opts;
    opts.source_official = false;
    opts.yes = false;
    opts.dry_run = false;
    opts.bp_source_name = "third_party";

    // Empty stdin = immediate EOF; apply's `getline(std::cin, ...)` returns
    // false → trust gate maps to "decline → abort".
    CinRedirect cin_eof("");
    auto r = bpa::apply(spec, lock, opts);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().find("aborted") != std::string::npos);
    CHECK_FALSE(fs::exists(luban::paths::home() / "nonofficial-eof-target.txt"));
    CHECK_FALSE(luban::applied_db::is_applied(spec.name));
}

TEST_CASE("apply with non-official source + interactive `y` proceeds") {
    Sandbox sb;
    auto spec = make_one_file_spec("nonofficial-y");
    bpl::BlueprintLock lock;
    lock.blueprint_name = spec.name;

    bpa::ApplyOptions opts;
    opts.source_official = false;
    opts.yes = false;
    opts.dry_run = false;
    opts.bp_source_name = "third_party";

    CinRedirect cin_yes("y\n");
    auto r = bpa::apply(spec, lock, opts);
    REQUIRE(r.has_value());
    CHECK(fs::exists(luban::paths::home() / "nonofficial-y-target.txt"));
}
