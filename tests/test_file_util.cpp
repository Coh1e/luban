// Tests for src/file_util.cpp — read_text / read_text_no_bom /
// write_text_atomic. These three helpers replaced near-identical copies
// previously scattered across specs.cpp, new_project.cpp, and
// manifest_source.cpp; the BOM-stripping version is the one most likely
// to regress (it's what JSON manifest parsing ultimately depends on).

#include "doctest.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "file_util.hpp"

namespace fs = std::filesystem;

namespace {

fs::path tmp_path(const std::string& tag) {
    return fs::temp_directory_path() /
           ("luban-fileutil-test-" + tag + "-" +
            std::to_string(std::hash<std::string>{}(tag)));
}

}  // namespace

TEST_CASE("file_util::read_text returns empty string on missing file") {
    auto p = tmp_path("read-missing");
    fs::remove(p);
    CHECK(luban::file_util::read_text(p).empty());
}

TEST_CASE("file_util::read_text returns full content of a small text file") {
    auto p = tmp_path("read-small");
    {
        std::ofstream out(p, std::ios::binary);
        out << "hello luban";
    }
    CHECK(luban::file_util::read_text(p) == "hello luban");
    fs::remove(p);
}

TEST_CASE("file_util::read_text_no_bom strips a leading UTF-8 BOM") {
    // Notepad / VS Code save UTF-8 with a BOM by default. nlohmann::json
    // and toml++ both reject the BOM as invalid input. read_text_no_bom
    // is what manifest parsers call to be resilient to this.
    auto p = tmp_path("read-bom");
    {
        std::ofstream out(p, std::ios::binary);
        out.put(static_cast<char>(0xEF));
        out.put(static_cast<char>(0xBB));
        out.put(static_cast<char>(0xBF));
        out << "hello luban";
    }
    auto s = luban::file_util::read_text_no_bom(p);
    CHECK(s == "hello luban");  // BOM gone
    CHECK(s.size() == 11);       // not 14
    fs::remove(p);
}

TEST_CASE("file_util::read_text_no_bom leaves non-BOM content untouched") {
    auto p = tmp_path("read-nobom");
    {
        std::ofstream out(p, std::ios::binary);
        out << "no BOM here";
    }
    CHECK(luban::file_util::read_text_no_bom(p) == "no BOM here");
    fs::remove(p);
}

TEST_CASE("file_util::read_text_no_bom does not strip mid-file 0xEF bytes") {
    // Only the very first 3 bytes count as a BOM — a 0xEF later in the
    // file (e.g., a multi-byte UTF-8 codepoint at offset N) must survive.
    auto p = tmp_path("read-midbom");
    {
        std::ofstream out(p, std::ios::binary);
        out << "abc";
        out.put(static_cast<char>(0xEF));
        out.put(static_cast<char>(0xBB));
        out.put(static_cast<char>(0xBF));
        out << "def";
    }
    auto s = luban::file_util::read_text_no_bom(p);
    CHECK(s.size() == 9);  // 3 + 3 (mid-bytes preserved) + 3
    CHECK(s.substr(0, 3) == "abc");
    fs::remove(p);
}

TEST_CASE("file_util::write_text_atomic writes content + creates parents") {
    // Atomic write must create missing parent dirs (callers rely on this
    // for `<state>/installed.json` etc. on a fresh machine).
    auto root = tmp_path("write-parents");
    auto p = root / "deep" / "nested" / "file.txt";
    fs::remove_all(root);

    REQUIRE(luban::file_util::write_text_atomic(p, "atomic content"));
    CHECK(fs::exists(p));
    CHECK(luban::file_util::read_text(p) == "atomic content");

    fs::remove_all(root);
}

TEST_CASE("file_util::write_text_atomic overwrites existing files") {
    auto p = tmp_path("write-overwrite");
    REQUIRE(luban::file_util::write_text_atomic(p, "v1"));
    REQUIRE(luban::file_util::write_text_atomic(p, "v2 longer"));
    CHECK(luban::file_util::read_text(p) == "v2 longer");
    fs::remove(p);
}

TEST_CASE("file_util::write_text_atomic leaves no .tmp file on success") {
    // tmp+rename pattern: the .tmp file must be gone after success
    // (otherwise repeated writes leak temp clutter).
    auto p = tmp_path("write-no-tmp");
    fs::path tmp = p; tmp += ".tmp";
    REQUIRE(luban::file_util::write_text_atomic(p, "clean"));
    CHECK(fs::exists(p));
    CHECK_FALSE(fs::exists(tmp));
    fs::remove(p);
}

TEST_CASE("file_util::write_text_atomic accepts string_view (not just std::string)") {
    // Compile-time check the signature: a string literal is implicitly
    // convertible. The previous per-module helpers each took different
    // argument types; standardizing on string_view is the whole point.
    auto p = tmp_path("write-sv");
    REQUIRE(luban::file_util::write_text_atomic(p, "literal"));
    CHECK(luban::file_util::read_text(p) == "literal");

    std::string s = "owned";
    REQUIRE(luban::file_util::write_text_atomic(p, s));
    CHECK(luban::file_util::read_text(p) == "owned");

    fs::remove(p);
}
