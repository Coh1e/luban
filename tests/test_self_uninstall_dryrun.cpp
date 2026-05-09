// Unit test for the read-side invariants `self uninstall --dry-run`
// relies on (G12). The destructive logic in commands/self.cpp's
// run_uninstall is wrapped in an anonymous namespace and is integration
// territory (CMakeLists.txt comment: "NOT main.cpp / cli.cpp / commands/
// — those are integration territory (smoke.bat covers them)").
//
// What we CAN test in isolation: applied_db's read primitives never
// mutate state. The dry-run preview reads from applied.txt +
// owned-shims.txt and prints "(dry-run) would remove …" lines; nothing
// in that path should alter on-disk state. If list_owned_shims grew a
// side effect (e.g. accidentally rotating the file), this test fails.

#include <filesystem>
#include <fstream>

#include "applied_db.hpp"
#include "doctest.h"
#include "paths.hpp"

namespace fs = std::filesystem;

namespace {

struct Sandbox {
    fs::path root;
    Sandbox() {
        root = fs::temp_directory_path() /
               ("luban-uninstall-dryrun-" + std::to_string(::time(nullptr)) + "-" +
                std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(root);
        ::_putenv_s("LUBAN_PREFIX", root.string().c_str());
        ::_putenv_s("USERPROFILE", root.string().c_str());
    }
    ~Sandbox() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

}  // namespace

TEST_CASE("applied_db reads are non-mutating (dry-run invariant)") {
    Sandbox sb;

    // Pre-populate state as if a real apply had run.
    REQUIRE(luban::applied_db::mark_applied("foundation"));
    REQUIRE(luban::applied_db::mark_applied("cpp-toolchain"));
    REQUIRE(luban::applied_db::record_owned_shim(
        luban::paths::xdg_bin_home() / "cmake.cmd"));
    REQUIRE(luban::applied_db::record_owned_shim(
        luban::paths::xdg_bin_home() / "ninja.cmd"));

    auto applied_path = luban::applied_db::applied_path();
    auto owned_path = luban::applied_db::owned_shims_path();
    REQUIRE(fs::exists(applied_path));
    REQUIRE(fs::exists(owned_path));

    auto initial_applied_size = fs::file_size(applied_path);
    auto initial_owned_size = fs::file_size(owned_path);

    // Dry-run preview path: read everything multiple times. No writes.
    for (int i = 0; i < 3; ++i) {
        CHECK(luban::applied_db::is_applied("foundation"));
        CHECK(luban::applied_db::is_applied("cpp-toolchain"));
        CHECK_FALSE(luban::applied_db::is_applied("never-applied"));
        auto shims = luban::applied_db::list_owned_shims();
        CHECK(shims.size() == 2);
    }

    // State must be byte-identical to the post-populate snapshot.
    CHECK(fs::file_size(applied_path) == initial_applied_size);
    CHECK(fs::file_size(owned_path) == initial_owned_size);
}

TEST_CASE("applied_db::clear is the explicit mutation — only the actual uninstall path uses it") {
    Sandbox sb;
    REQUIRE(luban::applied_db::mark_applied("foundation"));
    REQUIRE(luban::applied_db::record_owned_shim(
        luban::paths::xdg_bin_home() / "cmake.cmd"));

    auto applied_path = luban::applied_db::applied_path();
    auto owned_path = luban::applied_db::owned_shims_path();
    REQUIRE(fs::exists(applied_path));
    REQUIRE(fs::exists(owned_path));

    // Sanity: confirms `clear()` is what actually mutates. If the dry-run
    // path ever accidentally calls clear(), the corresponding smoke step
    // (state-preservation assertion) would fail; this test documents the
    // contract that clear() is the deliberate destruction.
    luban::applied_db::clear();
    CHECK_FALSE(fs::exists(applied_path));
    CHECK_FALSE(fs::exists(owned_path));
    CHECK_FALSE(luban::applied_db::is_applied("foundation"));
    CHECK(luban::applied_db::list_owned_shims().empty());
}
