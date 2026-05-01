// Tests for src/selection.cpp — toolchain selection JSON read/write +
// enable/disable mutation. selection.json is what `luban setup --with X`
// and `--without X` persist into; smoke.bat exercises --with end-to-end
// but never disable, so the cases below specifically pin the enable/
// disable contract that build_project.cpp (well, actually setup.cpp)
// relies on for "did we mutate? then save".

#include "doctest.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "selection.hpp"

namespace fs = std::filesystem;

// ---- enable -----------------------------------------------------------------

TEST_CASE("selection::enable on a missing name auto-adds to extras") {
    // `luban setup --with doxygen` on a manifest_seed where doxygen is
    // not pre-listed: enable() pushes a fresh entry into extras and
    // reports the change. component.cpp::install then walks selection
    // again to install only the "enabled" entries.
    luban::selection::Selection sel;
    REQUIRE(luban::selection::enable(sel, "doxygen"));
    REQUIRE(sel.extras.size() == 1);
    CHECK(sel.extras[0].name == "doxygen");
    CHECK(sel.extras[0].enabled);
}

TEST_CASE("selection::enable on a disabled extras entry flips it on") {
    luban::selection::Selection sel;
    sel.extras.push_back({"node", false, ""});
    REQUIRE(luban::selection::enable(sel, "node"));
    CHECK(sel.extras[0].enabled);
}

TEST_CASE("selection::enable on an already-enabled entry returns false (no-op)") {
    // Drives the "did we mutate? save?" check in setup.cpp — false means
    // "no change", so save() can be skipped, avoiding unnecessary disk
    // writes on idempotent re-runs.
    luban::selection::Selection sel;
    sel.components.push_back({"cmake", true, "build generator"});
    CHECK_FALSE(luban::selection::enable(sel, "cmake"));
    REQUIRE(sel.components.size() == 1);
    CHECK(sel.components[0].enabled);
}

TEST_CASE("selection::enable preserves note on existing entries (no clobber)") {
    luban::selection::Selection sel;
    sel.extras.push_back({"vcpkg", false, "C++ deps via Microsoft port collection"});
    REQUIRE(luban::selection::enable(sel, "vcpkg"));
    REQUIRE(sel.extras.size() == 1);
    CHECK(sel.extras[0].enabled);
    CHECK(sel.extras[0].note == "C++ deps via Microsoft port collection");
}

// ---- disable ----------------------------------------------------------------

TEST_CASE("selection::disable on an enabled entry flips it off and reports change") {
    luban::selection::Selection sel;
    sel.extras.push_back({"emscripten", true, ""});
    REQUIRE(luban::selection::disable(sel, "emscripten"));
    CHECK_FALSE(sel.extras[0].enabled);
}

TEST_CASE("selection::disable on an already-disabled entry returns false") {
    luban::selection::Selection sel;
    sel.extras.push_back({"emscripten", false, ""});
    CHECK_FALSE(luban::selection::disable(sel, "emscripten"));
    CHECK_FALSE(sel.extras[0].enabled);  // unchanged
}

TEST_CASE("selection::disable on a non-existent name returns false (no auto-add)") {
    // Asymmetric with enable() on purpose: --without an unknown name is
    // a user typo, not an instruction to materialize a new dummy entry.
    luban::selection::Selection sel;
    CHECK_FALSE(luban::selection::disable(sel, "ghost-tool"));
    CHECK(sel.extras.empty());
    CHECK(sel.components.empty());
}

TEST_CASE("selection::disable scans both components and extras") {
    // setup --without applies regardless of which list the entry lives
    // in. Components are typically core toolchain (cmake/ninja/llvm-mingw),
    // extras are opt-in (vcpkg, emscripten, doxygen).
    luban::selection::Selection sel;
    sel.components.push_back({"ninja", true, ""});
    sel.extras.push_back({"vcpkg", true, ""});

    REQUIRE(luban::selection::disable(sel, "ninja"));
    REQUIRE(luban::selection::disable(sel, "vcpkg"));
    CHECK_FALSE(sel.components[0].enabled);
    CHECK_FALSE(sel.extras[0].enabled);
}

// ---- enable then disable round-trip ----------------------------------------

TEST_CASE("selection::enable / disable reach a consistent in-memory state") {
    // Simulates: `luban setup --with emscripten`, then later
    // `luban setup --without emscripten`. The entry should remain in
    // extras (so we know luban knows about it) but be disabled.
    luban::selection::Selection sel;
    REQUIRE(luban::selection::enable(sel, "emscripten"));
    REQUIRE(sel.extras.size() == 1);
    CHECK(sel.extras[0].enabled);

    REQUIRE(luban::selection::disable(sel, "emscripten"));
    REQUIRE(sel.extras.size() == 1);
    CHECK_FALSE(sel.extras[0].enabled);

    // Calling disable again is a no-op (idempotent).
    CHECK_FALSE(luban::selection::disable(sel, "emscripten"));
}

// ---- ordering invariant ----------------------------------------------------

TEST_CASE("selection::enable preserves existing ordering when entry already exists") {
    // setup.cpp iterates components+extras in order and installs in that
    // order — node must be installed before emscripten because emscripten
    // depends on it. Mutating an existing entry must NOT reorder.
    luban::selection::Selection sel;
    sel.extras.push_back({"node",       false, ""});
    sel.extras.push_back({"emscripten", false, ""});
    REQUIRE(luban::selection::enable(sel, "emscripten"));
    REQUIRE(sel.extras.size() == 2);
    CHECK(sel.extras[0].name == "node");
    CHECK(sel.extras[1].name == "emscripten");
    CHECK(sel.extras[1].enabled);
}
