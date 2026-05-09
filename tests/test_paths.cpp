// Tests for src/paths.cpp — XDG-first directory resolution.
//
// paths.cpp is the only owner of "where does luban put X?" logic; every
// other module pulls from here. M4 Phase A (POSIX port, ADR-0006) will
// modify several of these branches (config_dir's localappdata fallback,
// home() on macOS, the resolve() platform dispatch). The cases below
// pin the resolution contract before that refactor lands so the port
// is mechanically verifiable.
//
// Strategy: each test sets/unsets env vars (XDG_*, LUBAN_PREFIX, HOME,
// USERPROFILE), calls the dir helper, checks the result, then restores
// the saved env. paths::* read env at call time, not init time, so this
// works without any "reload paths" plumbing.

#include "doctest.h"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

#include "paths.hpp"

namespace fs = std::filesystem;

namespace {

// RAII env override — sets a var on construction, restores prior value
// (including "was unset") on destruction. Unsetting via empty string
// works for both _putenv_s (Windows) and setenv (POSIX) the way paths.cpp
// reads env: `from_env` treats whitespace-only / empty as missing.
class EnvOverride {
public:
    EnvOverride(const char* name, const std::string& value) : name_(name) {
        const char* prev = std::getenv(name);
        had_prev_ = (prev != nullptr);
        if (had_prev_) prev_value_ = prev;
        set(value);
    }
    ~EnvOverride() {
        if (had_prev_) set(prev_value_);
        else unset();
    }
    EnvOverride(const EnvOverride&) = delete;
    EnvOverride& operator=(const EnvOverride&) = delete;

private:
    void set(const std::string& v) {
        _putenv_s(name_, v.c_str());
    }
    void unset() {
        // Windows: empty value = unset for the purposes of from_env, since
        // raw_env's GetEnvironmentVariableW returns 0 length for "".
        _putenv_s(name_, "");
    }
    const char* name_;
    bool had_prev_ = false;
    std::string prev_value_;
};

// Many tests need to clear several XDG vars at once so the resolve()
// path doesn't pick a leftover from the host env.
struct ScopedXdgClear {
    EnvOverride data{"XDG_DATA_HOME", ""};
    EnvOverride cache{"XDG_CACHE_HOME", ""};
    EnvOverride state{"XDG_STATE_HOME", ""};
    EnvOverride config{"XDG_CONFIG_HOME", ""};
    EnvOverride prefix{"LUBAN_PREFIX", ""};
};

}  // namespace

// ---- from_env ---------------------------------------------------------------

TEST_CASE("paths::from_env returns nullopt for an unset var") {
    EnvOverride v("LUBAN_TEST_UNSET_XYZQ", "");
    CHECK_FALSE(luban::paths::from_env("LUBAN_TEST_UNSET_XYZQ").has_value());
}

TEST_CASE("paths::from_env returns the literal value for a set var") {
    EnvOverride v("LUBAN_TEST_SET_XYZQ", "C:/foo/bar");
    auto p = luban::paths::from_env("LUBAN_TEST_SET_XYZQ");
    REQUIRE(p.has_value());
    CHECK(p->generic_string() == "C:/foo/bar");
}

TEST_CASE("paths::from_env trims surrounding whitespace") {
    EnvOverride v("LUBAN_TEST_TRIM_XYZQ", "  /tmp/with-spaces  ");
    auto p = luban::paths::from_env("LUBAN_TEST_TRIM_XYZQ");
    REQUIRE(p.has_value());
    CHECK(p->generic_string() == "/tmp/with-spaces");
}

TEST_CASE("paths::from_env treats whitespace-only as missing") {
    EnvOverride v("LUBAN_TEST_BLANK_XYZQ", "   \t  ");
    CHECK_FALSE(luban::paths::from_env("LUBAN_TEST_BLANK_XYZQ").has_value());
}

TEST_CASE("paths::from_env expands a leading ~ to home") {
    EnvOverride v("LUBAN_TEST_TILDE_XYZQ", "~/projects/luban");
    auto p = luban::paths::from_env("LUBAN_TEST_TILDE_XYZQ");
    REQUIRE(p.has_value());
    // The expansion concatenates `home() / "projects/luban"`, so the
    // result should at minimum end with the tail and not literally
    // start with `~`.
    CHECK(p->generic_string().find('~') == std::string::npos);
    CHECK(p->generic_string().find("projects/luban") != std::string::npos);
}

// ---- LUBAN_PREFIX (container / CI override) ---------------------------------

TEST_CASE("paths::data_dir honours LUBAN_PREFIX (container/CI override)") {
    // LUBAN_PREFIX is the all-in-one knob for ephemeral environments
    // (CI runners, docker images): set it and all 4 homes land under
    // <prefix>/{data,cache,state,config}.
    ScopedXdgClear clear;
    EnvOverride prefix("LUBAN_PREFIX", "C:/luban-prefix-test");
    auto p = luban::paths::data_dir();
    CHECK(p.generic_string() == "C:/luban-prefix-test/data");
}

TEST_CASE("paths::cache_dir honours LUBAN_PREFIX") {
    ScopedXdgClear clear;
    EnvOverride prefix("LUBAN_PREFIX", "C:/luban-prefix-test");
    CHECK(luban::paths::cache_dir().generic_string() == "C:/luban-prefix-test/cache");
}

TEST_CASE("paths::state_dir honours LUBAN_PREFIX") {
    ScopedXdgClear clear;
    EnvOverride prefix("LUBAN_PREFIX", "C:/luban-prefix-test");
    CHECK(luban::paths::state_dir().generic_string() == "C:/luban-prefix-test/state");
}

TEST_CASE("paths::config_dir honours LUBAN_PREFIX") {
    ScopedXdgClear clear;
    EnvOverride prefix("LUBAN_PREFIX", "C:/luban-prefix-test");
    CHECK(luban::paths::config_dir().generic_string() == "C:/luban-prefix-test/config");
}

// ---- XDG_* overrides --------------------------------------------------------

TEST_CASE("paths::data_dir honours XDG_DATA_HOME (and appends APP_NAME)") {
    // XDG conventions specify per-user data home; luban appends its own
    // APP_NAME ("luban") subdir so multiple XDG-aware tools coexist.
    ScopedXdgClear clear;
    EnvOverride data("XDG_DATA_HOME", "C:/xdg-data");
    auto p = luban::paths::data_dir();
    CHECK(p.generic_string() == "C:/xdg-data/luban");
}

TEST_CASE("paths::cache_dir honours XDG_CACHE_HOME") {
    ScopedXdgClear clear;
    EnvOverride cache("XDG_CACHE_HOME", "C:/xdg-cache");
    CHECK(luban::paths::cache_dir().generic_string() == "C:/xdg-cache/luban");
}

TEST_CASE("paths::state_dir honours XDG_STATE_HOME") {
    ScopedXdgClear clear;
    EnvOverride state("XDG_STATE_HOME", "C:/xdg-state");
    CHECK(luban::paths::state_dir().generic_string() == "C:/xdg-state/luban");
}

TEST_CASE("paths::config_dir honours XDG_CONFIG_HOME") {
    ScopedXdgClear clear;
    EnvOverride config("XDG_CONFIG_HOME", "C:/xdg-config");
    CHECK(luban::paths::config_dir().generic_string() == "C:/xdg-config/luban");
}

// ---- LUBAN_PREFIX > XDG_* > platform fallback ordering ----------------------

TEST_CASE("paths: LUBAN_PREFIX wins over XDG_* when both set") {
    // When CI sets LUBAN_PREFIX *and* the host already has XDG_DATA_HOME,
    // LUBAN_PREFIX must win (it's the explicit "use this isolated dir"
    // signal). The order is encoded as luban_prefix() check first in
    // paths.cpp::resolve().
    ScopedXdgClear clear;
    EnvOverride prefix("LUBAN_PREFIX", "C:/prefix-wins");
    EnvOverride data("XDG_DATA_HOME", "C:/should-be-ignored");
    CHECK(luban::paths::data_dir().generic_string() == "C:/prefix-wins/data");
}

// ---- Derived paths ----------------------------------------------------------

TEST_CASE("paths: bin_dir is data_dir/bin (cargo-style)") {
    // ADR: shims live under <data>/bin/, not ~/.local/bin/. Dropping ~280
    // LLVM-MinGW shims into the shared XDG bin would stress Defender +
    // clobber the dir's volume.
    ScopedXdgClear clear;
    EnvOverride prefix("LUBAN_PREFIX", "C:/p");
    CHECK(luban::paths::bin_dir().generic_string() == "C:/p/data/bin");
}

TEST_CASE("paths: vcpkg_downloads_dir nests under vcpkg_cache_dir under cache") {
    // vcpkg's three caches must each be real directories on launch
    // (vcpkg panics with "Value of X_VCPKG_REGISTRIES_CACHE is not a
    // directory" otherwise). The nesting is fixed: cache/vcpkg/{downloads,
    // archives, registries}.
    ScopedXdgClear clear;
    EnvOverride prefix("LUBAN_PREFIX", "C:/p");
    CHECK(luban::paths::vcpkg_cache_dir().generic_string()       == "C:/p/cache/vcpkg");
    CHECK(luban::paths::vcpkg_downloads_dir().generic_string()   == "C:/p/cache/vcpkg/downloads");
    CHECK(luban::paths::vcpkg_archives_dir().generic_string()    == "C:/p/cache/vcpkg/archives");
    CHECK(luban::paths::vcpkg_registries_dir().generic_string()  == "C:/p/cache/vcpkg/registries");
}

TEST_CASE("paths: installed_json_path is state_dir/installed.json") {
    ScopedXdgClear clear;
    EnvOverride prefix("LUBAN_PREFIX", "C:/p");
    CHECK(luban::paths::installed_json_path().generic_string() == "C:/p/state/installed.json");
}

TEST_CASE("paths: selection_json_path is config_dir/selection.json") {
    ScopedXdgClear clear;
    EnvOverride prefix("LUBAN_PREFIX", "C:/p");
    CHECK(luban::paths::selection_json_path().generic_string() == "C:/p/config/selection.json");
}

TEST_CASE("paths: toolchain_dir(name) = data_dir/toolchains/<name>") {
    ScopedXdgClear clear;
    EnvOverride prefix("LUBAN_PREFIX", "C:/p");
    CHECK(luban::paths::toolchain_dir("cmake-4.3.2").generic_string()
          == "C:/p/data/toolchains/cmake-4.3.2");
}

// ---- all_dirs ---------------------------------------------------------------

TEST_CASE("paths::all_dirs includes the four homes and key sub-dirs") {
    // doctor / setup walk all_dirs(); a regression that drops one entry
    // would silently break "luban setup --force" idempotency or
    // ensure_dirs creating vcpkg's cache subdirs.
    ScopedXdgClear clear;
    EnvOverride prefix("LUBAN_PREFIX", "C:/p");
    auto entries = luban::paths::all_dirs();

    auto has = [&](const std::string& label) {
        for (auto& [k, _] : entries) if (k == label) return true;
        return false;
    };
    CHECK(has("data"));
    CHECK(has("cache"));
    CHECK(has("state"));
    CHECK(has("config"));
    CHECK(has("bin"));
    CHECK(has("toolchains"));
    CHECK(has("vcpkg/downloads"));
    CHECK(has("vcpkg/archives"));
    CHECK(has("vcpkg/registries"));
    CHECK(has("logs"));
}

// ---- same_volume (Win32-specific behavior) ----------------------------------

TEST_CASE("paths::same_volume: same drive letter → true") {
    CHECK(luban::paths::same_volume("C:/foo/bar", "C:/baz"));
}

TEST_CASE("paths::same_volume: different drive letters → false") {
    CHECK_FALSE(luban::paths::same_volume("C:/foo", "D:/bar"));
}

TEST_CASE("paths::same_volume: case-insensitive root comparison") {
    // Drive letters on Windows are case-insensitive — "c:\\" and "C:\\"
    // are the same volume.
    CHECK(luban::paths::same_volume("c:/foo", "C:/bar"));
}
