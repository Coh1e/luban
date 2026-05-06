// Tests for src/archive.cpp — ZIP extraction with safety guards.
//
// archive.cpp is a security boundary for every component install: any
// regression in path-traversal protection or archive-type rejection lets
// a malicious manifest write outside <data>/toolchains. The cases below
// pin the four documented invariants:
//
//   1. UnsafeEntry on `..` segments / absolute paths / drive-letter prefixes
//   2. Unsupported on `.7z` / `.exe` / `.msi` extensions (self-extracting)
//   3. Single-top-level-wrapper flattening (`archive/foo-1.0/bin/x` → `dest/bin/x`)
//   4. Plain content extraction round-trips
//
// Test ZIPs are built in-memory via miniz's writer API. miniz is already
// vendored at third_party/miniz.{h,c}; the writer surface (mz_zip_writer_*)
// is the same TU that the production parser uses, so fixtures are bit-
// for-bit what real installer manifests would feed us.

#include "doctest.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "miniz.h"
#include "archive.hpp"

namespace fs = std::filesystem;

namespace {

fs::path tmp_path(const std::string& tag) {
    return fs::temp_directory_path() /
           ("luban-archive-test-" + tag + "-" +
            std::to_string(std::hash<std::string>{}(
                tag + std::to_string(reinterpret_cast<uintptr_t>(&tag)))));
}

// Tiny fixture builder. Writes a zip to `path` containing the given
// (entry_name, content) pairs. mz_zip_writer_add_mem stores the entry's
// archive-name verbatim — including unsafe forms like "../escape" if the
// caller passes that — which is exactly what we want for safety tests.
bool make_zip(const fs::path& path,
              const std::vector<std::pair<std::string, std::string>>& entries) {
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_file(&zip, path.string().c_str(), 0)) return false;
    for (auto& [name, content] : entries) {
        if (!mz_zip_writer_add_mem(&zip, name.c_str(),
                                    content.data(), content.size(),
                                    MZ_DEFAULT_COMPRESSION)) {
            mz_zip_writer_end(&zip);
            return false;
        }
    }
    if (!mz_zip_writer_finalize_archive(&zip)) { mz_zip_writer_end(&zip); return false; }
    if (!mz_zip_writer_end(&zip)) return false;
    return true;
}

std::string read_text_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

}  // namespace

// ---- happy path: plain extraction round-trips ------------------------------

TEST_CASE("archive::extract round-trips a small zip with two entries") {
    auto zip = tmp_path("plain") / "src.zip";
    auto dest = tmp_path("plain") / "dest";
    fs::create_directories(zip.parent_path());

    REQUIRE(make_zip(zip, {
        {"hello.txt",          "hello luban"},
        {"sub/world.txt",      "world inside sub"},
    }));

    auto rc = luban::archive::extract(zip, dest);
    REQUIRE(rc.has_value());
    CHECK(read_text_file(dest / "hello.txt")     == "hello luban");
    CHECK(read_text_file(dest / "sub" / "world.txt") == "world inside sub");

    fs::remove_all(zip.parent_path());
}

// ---- safety: path-traversal protection -------------------------------------

TEST_CASE("archive::extract rejects entry containing '..' segment") {
    // The classic zip-slip: `../etc/passwd` would overwrite a sibling of
    // dest. archive.cpp::is_unsafe_entry catches this *before* miniz gets
    // a chance to write — extraction must NOT have produced the file.
    auto zip = tmp_path("traversal") / "evil.zip";
    auto dest = tmp_path("traversal") / "dest";
    fs::create_directories(zip.parent_path());

    REQUIRE(make_zip(zip, {{"../escape.txt", "you should never see this"}}));

    auto rc = luban::archive::extract(zip, dest);
    REQUIRE_FALSE(rc.has_value());
    CHECK(rc.error().kind == luban::archive::ErrorKind::UnsafeEntry);
    // The escape.txt must not exist anywhere near dest.
    CHECK_FALSE(fs::exists(zip.parent_path() / "escape.txt"));
    CHECK_FALSE(fs::exists(dest / ".." / "escape.txt"));

    fs::remove_all(zip.parent_path());
}

// Note: miniz's writer (mz_zip_writer_add_mem) defensively rejects entry
// names starting with '/' before they hit the archive — so we can't
// construct the leading-/ test case from inside this TU. The drive-letter
// (`C:/...`) and backslash-absolute tests below cover the equivalent
// invariant from the consumer side; if a real-world zip with leading /
// reaches us, archive::is_unsafe_entry catches it via name.front() == '/'.

TEST_CASE("archive::extract rejects entry with absolute Windows path (drive letter)") {
    // `C:/foo` — `name[1] == ':'` heuristic in is_unsafe_entry. Real-world
    // attack vector: a malicious manifest whose archive contains
    // `C:/Windows/System32/...` would otherwise overwrite system files.
    auto zip = tmp_path("abs-win") / "evil.zip";
    auto dest = tmp_path("abs-win") / "dest";
    fs::create_directories(zip.parent_path());

    REQUIRE(make_zip(zip, {{"C:/foo.txt", "haha"}}));

    auto rc = luban::archive::extract(zip, dest);
    REQUIRE_FALSE(rc.has_value());
    CHECK(rc.error().kind == luban::archive::ErrorKind::UnsafeEntry);

    fs::remove_all(zip.parent_path());
}

TEST_CASE("archive::extract rejects '..' nested deep, not just at start") {
    // `subdir/../../escape` — the `..` is the second segment, but it's
    // still a traversal once `subdir` is consumed. is_unsafe_entry checks
    // every segment, not just the first.
    auto zip = tmp_path("nested-dotdot") / "evil.zip";
    auto dest = tmp_path("nested-dotdot") / "dest";
    fs::create_directories(zip.parent_path());

    REQUIRE(make_zip(zip, {{"subdir/../../escape.txt", "haha"}}));

    auto rc = luban::archive::extract(zip, dest);
    REQUIRE_FALSE(rc.has_value());
    CHECK(rc.error().kind == luban::archive::ErrorKind::UnsafeEntry);

    fs::remove_all(zip.parent_path());
}

TEST_CASE("archive::extract rejects backslash absolute path (Windows-style)") {
    // miniz normalizes separators when reading, but is_unsafe_entry runs
    // on the raw entry name. Both `/` and `\\` at position 0 must be
    // rejected for symmetry.
    auto zip = tmp_path("backslash-abs") / "evil.zip";
    auto dest = tmp_path("backslash-abs") / "dest";
    fs::create_directories(zip.parent_path());

    REQUIRE(make_zip(zip, {{"\\etc\\passwd", "haha"}}));

    auto rc = luban::archive::extract(zip, dest);
    REQUIRE_FALSE(rc.has_value());
    CHECK(rc.error().kind == luban::archive::ErrorKind::UnsafeEntry);

    fs::remove_all(zip.parent_path());
}

// ---- safety: archive-type rejection ----------------------------------------

TEST_CASE("archive::extract rejects .7z extension") {
    // Even if the file *content* were a valid zip, the .7z extension
    // earns an Unsupported error before miniz looks at it. Reasoning: a
    // malicious manifest could double-extension `foo.zip.7z` to bypass
    // the extension check; we belt-and-suspenders.
    auto zip = tmp_path("7z-ext") / "fake.7z";
    auto dest = tmp_path("7z-ext") / "dest";
    fs::create_directories(zip.parent_path());

    // Write a trivially-valid zip body, but with .7z extension.
    REQUIRE(make_zip(zip, {{"x.txt", "x"}}));

    auto rc = luban::archive::extract(zip, dest);
    REQUIRE_FALSE(rc.has_value());
    CHECK(rc.error().kind == luban::archive::ErrorKind::Unsupported);

    fs::remove_all(zip.parent_path());
}

TEST_CASE("archive::extract rejects .exe extension (self-extracting installer)") {
    auto zip = tmp_path("exe-ext") / "fake.exe";
    auto dest = tmp_path("exe-ext") / "dest";
    fs::create_directories(zip.parent_path());
    REQUIRE(make_zip(zip, {{"x.txt", "x"}}));

    auto rc = luban::archive::extract(zip, dest);
    REQUIRE_FALSE(rc.has_value());
    CHECK(rc.error().kind == luban::archive::ErrorKind::Unsupported);

    fs::remove_all(zip.parent_path());
}

TEST_CASE("archive::extract rejects .msi extension") {
    auto zip = tmp_path("msi-ext") / "fake.msi";
    auto dest = tmp_path("msi-ext") / "dest";
    fs::create_directories(zip.parent_path());
    REQUIRE(make_zip(zip, {{"x.txt", "x"}}));

    auto rc = luban::archive::extract(zip, dest);
    REQUIRE_FALSE(rc.has_value());
    CHECK(rc.error().kind == luban::archive::ErrorKind::Unsupported);

    fs::remove_all(zip.parent_path());
}

// ---- single-wrapper flattening ---------------------------------------------

TEST_CASE("archive::extract flattens a single top-level wrapper directory") {
    // ScoopInstaller manifests routinely package toolchains as
    // `cmake-4.3.2-windows-x86_64/bin/cmake.exe` etc. Without flatten,
    // the user's PATH would need to include the version-stamped subdir,
    // which luban's shim model doesn't track. Flatten lifts everything
    // up one level so `dest/bin/cmake.exe` works.
    auto zip = tmp_path("flatten") / "src.zip";
    auto dest = tmp_path("flatten") / "dest";
    fs::create_directories(zip.parent_path());

    REQUIRE(make_zip(zip, {
        {"cmake-4.3.2/bin/cmake.exe",       "fake exe content"},
        {"cmake-4.3.2/share/doc/README.txt", "readme"},
    }));

    auto rc = luban::archive::extract(zip, dest);
    REQUIRE(rc.has_value());
    // Files now at dest/bin/... not dest/cmake-4.3.2/bin/...
    CHECK(fs::exists(dest / "bin" / "cmake.exe"));
    CHECK(fs::exists(dest / "share" / "doc" / "README.txt"));
    CHECK_FALSE(fs::exists(dest / "cmake-4.3.2"));

    fs::remove_all(zip.parent_path());
}

TEST_CASE("archive::extract does NOT flatten when there are multiple top-level entries") {
    // If the archive root has two siblings, flattening would have to
    // pick one — better to do nothing and let the user see the structure.
    auto zip = tmp_path("no-flatten") / "src.zip";
    auto dest = tmp_path("no-flatten") / "dest";
    fs::create_directories(zip.parent_path());

    REQUIRE(make_zip(zip, {
        {"foo/file.txt", "foo content"},
        {"bar/file.txt", "bar content"},
    }));

    auto rc = luban::archive::extract(zip, dest);
    REQUIRE(rc.has_value());
    CHECK(fs::exists(dest / "foo" / "file.txt"));
    CHECK(fs::exists(dest / "bar" / "file.txt"));

    fs::remove_all(zip.parent_path());
}

// ---- error: missing archive ------------------------------------------------

TEST_CASE("archive::extract returns Io error for a missing archive file") {
    auto missing = tmp_path("missing") / "definitely-not-here.zip";
    auto dest    = tmp_path("missing") / "dest";

    auto rc = luban::archive::extract(missing, dest);
    REQUIRE_FALSE(rc.has_value());
    CHECK(rc.error().kind == luban::archive::ErrorKind::Io);
    CHECK(rc.error().message.find("not found") != std::string::npos);
}

// ---- error: corrupt zip ----------------------------------------------------

TEST_CASE("archive::extract returns Corrupt error for non-zip content with .zip extension") {
    // .zip extension means we trust miniz to handle it — when miniz
    // finds the file isn't actually a zip, archive returns Corrupt
    // (not Unsupported, since the user did claim "this is a zip").
    auto fake_zip = tmp_path("corrupt") / "fake.zip";
    auto dest     = tmp_path("corrupt") / "dest";
    fs::create_directories(fake_zip.parent_path());

    {
        std::ofstream out(fake_zip, std::ios::binary);
        out << "this is definitely not a zip file";
    }

    auto rc = luban::archive::extract(fake_zip, dest);
    REQUIRE_FALSE(rc.has_value());
    CHECK(rc.error().kind == luban::archive::ErrorKind::Corrupt);

    fs::remove_all(fake_zip.parent_path());
}

// ---- staging cleanup -------------------------------------------------------

TEST_CASE("archive::extract leaves no .luban-staging directory on success") {
    // Plan note: extract uses a `<dest>.luban-staging` sibling dir to
    // build up the result, then renames into place. On success the
    // staging dir must be gone (otherwise repeated installs accumulate
    // half-built leftovers).
    auto zip   = tmp_path("staging") / "src.zip";
    auto dest  = tmp_path("staging") / "dest";
    fs::create_directories(zip.parent_path());

    REQUIRE(make_zip(zip, {{"x.txt", "x"}}));
    REQUIRE(luban::archive::extract(zip, dest).has_value());

    fs::path staging = dest.parent_path() / (dest.filename().string() + ".luban-staging");
    CHECK_FALSE(fs::exists(staging));

    fs::remove_all(zip.parent_path());
}
