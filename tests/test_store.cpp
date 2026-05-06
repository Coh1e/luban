// Unit tests for src/store.cpp.
//
// We don't drive real downloads here — that's network territory and
// belongs in nightly integration tests. What we *do* exercise:
// - compute_artifact_id format + idempotence
// - store_path layout under <data>
// - is_present detection (with real file/dir manipulation in temp_dir)
//
// fetch() itself is integration-tested via smoke.bat once cpp-base.toml
// lands later in the v1.0 plan.

#include <filesystem>
#include <fstream>

#include "doctest.h"
#include "paths.hpp"
#include "store.hpp"

namespace fs = std::filesystem;

TEST_CASE("compute_artifact_id format") {
    auto id = luban::store::compute_artifact_id(
        "ripgrep", "14.0.3", "windows-x64",
        "abc123def456789012345678abcdef00");
    // Format: <name>-<version>-<target>-<hash8>; hash8 is first 8 hex
    // chars of sha256 (lowercased).
    CHECK(id == "ripgrep-14.0.3-windows-x64-abc123de");
}

TEST_CASE("compute_artifact_id with sha256: prefix on the hash") {
    // Some callers pass "sha256:abc..." style; compute_artifact_id should
    // strip the prefix and use only the hex portion.
    auto id = luban::store::compute_artifact_id(
        "x", "1.0", "linux-x64", "sha256:cafef00ddeadbeef");
    CHECK(id == "x-1.0-linux-x64-cafef00d");
}

TEST_CASE("compute_artifact_id uppercases hash → lowercase hash8") {
    // Hex input is sometimes uppercase; output should always be lower so
    // the dir name is stable across users / OSes that case-fold paths.
    auto id = luban::store::compute_artifact_id(
        "x", "1.0", "linux-x64", "DEADBEEF12345678");
    CHECK(id.find("deadbeef") != std::string::npos);
    CHECK(id.find("DEADBEEF") == std::string::npos);
}

TEST_CASE("compute_artifact_id substitutes empty version") {
    auto id = luban::store::compute_artifact_id(
        "ripgrep", "", "windows-x64", "abc12345abcdef00");
    // Empty version becomes "unversioned" so the result is still the
    // expected 4-segment shape.
    CHECK(id == "ripgrep-unversioned-windows-x64-abc12345");
}

TEST_CASE("compute_artifact_id passes hyphens through") {
    // Tool names + targets routinely contain hyphens (luban-shim, windows-x64,
    // x64-mingw-static). Downstream parsers anchor on the trailing 8-char
    // hash8 rather than splitting on '-', so we don't escape.
    auto id = luban::store::compute_artifact_id(
        "luban-shim", "0.1.0", "windows-x64", "12345678abcdef00");
    CHECK(id == "luban-shim-0.1.0-windows-x64-12345678");
}

TEST_CASE("compute_artifact_id pads short hash to 8 chars") {
    // Defensive: if some caller hands us a too-short hash, we shouldn't
    // emit a malformed artifact_id. Pad with zeros.
    auto id = luban::store::compute_artifact_id(
        "x", "1.0", "linux-x64", "abc");
    CHECK(id == "x-1.0-linux-x64-abc00000");
}

TEST_CASE("store_path returns under the data dir's store/ subdir") {
    auto path = luban::store::store_path("ripgrep-14.0.3-windows-x64-aabbccdd");
    auto expected = luban::paths::data_dir() / "store" /
                    "ripgrep-14.0.3-windows-x64-aabbccdd";
    CHECK(path == expected);
}

TEST_CASE("is_present false for nonexistent artifact") {
    CHECK_FALSE(luban::store::is_present(
        "this-definitely-does-not-exist-9876543210abcdef"));
}

TEST_CASE("is_present false when dir exists but has no marker") {
    // Manufacture a directory at the canonical store path WITHOUT a
    // marker file. This simulates an interrupted-mid-extract state and
    // proves is_present requires the marker.
    auto fake_id = std::string("test-store-no-marker-") +
                   std::to_string(::time(nullptr));
    auto dir = luban::store::store_path(fake_id);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!ec) {
        CHECK_FALSE(luban::store::is_present(fake_id));
        // Cleanup.
        fs::remove_all(dir, ec);
    }
}

TEST_CASE("is_present true when dir exists with marker file") {
    auto fake_id = std::string("test-store-with-marker-") +
                   std::to_string(::time(nullptr));
    auto dir = luban::store::store_path(fake_id);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!ec) {
        std::ofstream mf(dir / ".store-marker.json");
        mf << "{}";
        mf.close();
        CHECK(luban::store::is_present(fake_id));
        fs::remove_all(dir, ec);
    }
}
