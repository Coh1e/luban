// Unit tests for src/blueprint_lock.cpp.

#include <filesystem>
#include <fstream>
#include <string>

#include "blueprint_lock.hpp"
#include "doctest.h"

namespace bp = luban::blueprint;
namespace bpl = luban::blueprint_lock;

namespace {

bpl::BlueprintLock sample_lock() {
    bpl::BlueprintLock lock;
    lock.schema = 1;
    lock.blueprint_name = "cli-quality";
    lock.blueprint_sha256 = "sha256:abc123";
    lock.resolved_at = "2026-05-05T12:00:00Z";

    bpl::LockedTool rg;
    rg.version = "14.0.3";
    rg.source = "github:BurntSushi/ripgrep";
    bpl::LockedPlatform p;
    p.url = "https://github.com/BurntSushi/ripgrep/releases/download/14.0.3/x.zip";
    p.sha256 = "deadbeef";
    p.bin = "rg.exe";
    p.artifact_id = "ripgrep-14.0.3-windows-x64-aabbccdd";
    rg.platforms["windows-x64"] = p;
    lock.tools["ripgrep"] = std::move(rg);

    bpl::LockedFile f;
    f.content_sha256 = "cafef00d";
    f.mode = bp::FileMode::Replace;
    lock.files["~/.config/bat/config"] = std::move(f);

    return lock;
}

}  // namespace

TEST_CASE("to_string produces parseable JSON") {
    auto lock = sample_lock();
    std::string json_text = bpl::to_string(lock);

    // Spot-check key fields are in the output.
    CHECK(json_text.find("\"schema\": 1") != std::string::npos);
    CHECK(json_text.find("\"blueprint_name\": \"cli-quality\"") != std::string::npos);
    CHECK(json_text.find("\"version\": \"14.0.3\"") != std::string::npos);
    CHECK(json_text.find("\"windows-x64\"") != std::string::npos);
}

TEST_CASE("read_string round-trips to_string") {
    auto original = sample_lock();
    std::string json_text = bpl::to_string(original);

    auto parsed = bpl::read_string(json_text);
    REQUIRE(parsed.has_value());
    CHECK(parsed->schema == 1);
    CHECK(parsed->blueprint_name == "cli-quality");
    CHECK(parsed->blueprint_sha256 == "sha256:abc123");

    REQUIRE(parsed->tools.count("ripgrep") == 1);
    auto& rg = parsed->tools["ripgrep"];
    CHECK(rg.version == "14.0.3");
    CHECK(rg.source == "github:BurntSushi/ripgrep");
    REQUIRE(rg.platforms.count("windows-x64") == 1);
    auto& p = rg.platforms["windows-x64"];
    CHECK(p.sha256 == "deadbeef");
    CHECK(p.bin == "rg.exe");
    CHECK(p.artifact_id == "ripgrep-14.0.3-windows-x64-aabbccdd");

    REQUIRE(parsed->files.count("~/.config/bat/config") == 1);
    auto& f = parsed->files["~/.config/bat/config"];
    CHECK(f.mode == bp::FileMode::Replace);
    CHECK(f.content_sha256 == "cafef00d");
}

TEST_CASE("malformed JSON surfaces error") {
    auto r = bpl::read_string("{ this is not json");
    CHECK_FALSE(r.has_value());
    CHECK(r.error().find("JSON") != std::string::npos);
}

TEST_CASE("missing required field is an error") {
    auto r = bpl::read_string(R"({"schema": 1})");
    CHECK_FALSE(r.has_value());
    // missing blueprint_name should appear in the error
    CHECK(r.error().find("blueprint_name") != std::string::npos);
}

TEST_CASE("unsupported schema version is rejected") {
    auto r = bpl::read_string(R"({"schema": 999, "blueprint_name": "x",
        "blueprint_sha256": "y", "resolved_at": "z"})");
    CHECK_FALSE(r.has_value());
    CHECK(r.error().find("schema") != std::string::npos);
    CHECK(r.error().find("999") != std::string::npos);
}

TEST_CASE("drop-in mode round-trips") {
    bpl::BlueprintLock lock;
    lock.schema = 1;
    lock.blueprint_name = "x";
    lock.blueprint_sha256 = "y";
    lock.resolved_at = "z";
    bpl::LockedFile f;
    f.content_sha256 = "abc";
    f.mode = bp::FileMode::DropIn;
    lock.files["~/.gitconfig.d/aliases"] = std::move(f);

    auto json_text = bpl::to_string(lock);
    auto parsed = bpl::read_string(json_text);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->files.count("~/.gitconfig.d/aliases") == 1);
    CHECK(parsed->files["~/.gitconfig.d/aliases"].mode == bp::FileMode::DropIn);
}

TEST_CASE("write_file + read_file round-trip on disk") {
    auto path = std::filesystem::temp_directory_path() /
                "luban-test-blueprint-lock.json";
    auto original = sample_lock();

    auto w = bpl::write_file(path, original);
    REQUIRE(w.has_value());
    REQUIRE(std::filesystem::exists(path));

    auto parsed = bpl::read_file(path);
    REQUIRE(parsed.has_value());
    CHECK(parsed->blueprint_name == "cli-quality");
    REQUIRE(parsed->tools.count("ripgrep") == 1);

    // Cleanup — best-effort, doctest doesn't have a teardown facility.
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("write_file is atomic (no .tmp left behind)") {
    auto path = std::filesystem::temp_directory_path() /
                "luban-test-blueprint-lock-atomic.json";
    auto tmp_path = path;
    tmp_path += ".tmp";

    auto w = bpl::write_file(path, sample_lock());
    REQUIRE(w.has_value());

    // After a successful write, the .tmp must be gone (renamed away).
    CHECK_FALSE(std::filesystem::exists(tmp_path));
    CHECK(std::filesystem::exists(path));

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("read_file returns error for missing file") {
    auto bogus = std::filesystem::temp_directory_path() /
                 "luban-this-definitely-does-not-exist-12345.lock";
    auto r = bpl::read_file(bogus);
    CHECK_FALSE(r.has_value());
}
