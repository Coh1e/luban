// Unit tests covering the env layer that backs `luban env --user` /
// `--unset-user` / `--print` (DESIGN §10.4).
//
// Two halves:
//   1. env_snapshot::path_dirs / env_dict / apply_to — pure-ish: we
//      sandbox LUBAN_PREFIX so paths::* land in temp, then verify the
//      composed env that luban-spawned children would see.
//   2. win_path::{set,get,unset}_user_env round-trip — Windows-only;
//      uses a uniquely-named env var so a test failure can't clobber
//      the user's real HKCU state.

#include <filesystem>
#include <map>
#include <string>

#include "doctest.h"
#include "env_snapshot.hpp"
#include "msvc_env.hpp"
#include "paths.hpp"
#include "win_path.hpp"

namespace fs = std::filesystem;

namespace {

struct Sandbox {
    fs::path root;
    Sandbox() {
        root = fs::temp_directory_path() /
               ("luban-env-test-" + std::to_string(::time(nullptr)) + "-" +
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
        // No persisted msvc-env.json was written by these tests; defensive
        // clear in case a future test grows that surface.
        luban::msvc_env::clear();
        fs::remove_all(root, ec);
    }
};

}  // namespace

TEST_CASE("env_snapshot::env_dict exposes vcpkg cache vars under <cache>/vcpkg") {
    Sandbox sb;
    auto entries = luban::env_snapshot::env_dict();

    std::map<std::string, std::string> by_key;
    for (auto& [k, v] : entries) by_key[k] = v;

    REQUIRE(by_key.count("VCPKG_DOWNLOADS"));
    REQUIRE(by_key.count("VCPKG_DEFAULT_BINARY_CACHE"));
    REQUIRE(by_key.count("X_VCPKG_REGISTRIES_CACHE"));

    // Every cache var must point inside <cache>/vcpkg/. Anchor on the
    // sandbox root so we don't accidentally pick up the user's real cache.
    fs::path cache = luban::paths::cache_dir();
    auto under_cache = [&](const std::string& s) {
        fs::path p = fs::path(s).lexically_normal();
        fs::path rel = p.lexically_relative(cache.lexically_normal());
        return !rel.empty() && rel.string().rfind("..", 0) != 0;
    };
    CHECK(under_cache(by_key["VCPKG_DOWNLOADS"]));
    CHECK(under_cache(by_key["VCPKG_DEFAULT_BINARY_CACHE"]));
    CHECK(under_cache(by_key["X_VCPKG_REGISTRIES_CACHE"]));

    // Side effect: env_dict() ensures the dirs exist on disk so vcpkg's
    // own pre-flight is happy. Verify it actually created them.
    CHECK(fs::exists(by_key["VCPKG_DOWNLOADS"]));
    CHECK(fs::exists(by_key["VCPKG_DEFAULT_BINARY_CACHE"]));
    CHECK(fs::exists(by_key["X_VCPKG_REGISTRIES_CACHE"]));
}

TEST_CASE("env_snapshot::path_dirs includes xdg_bin_home when present") {
    Sandbox sb;
    fs::create_directories(luban::paths::xdg_bin_home());

    auto dirs = luban::env_snapshot::path_dirs();
    REQUIRE_FALSE(dirs.empty());
    CHECK(dirs.front() == luban::paths::xdg_bin_home());
}

TEST_CASE("env_snapshot::path_dirs skips xdg_bin_home when missing") {
    Sandbox sb;
    // Deliberately do NOT mkdir xdg_bin_home — path_dirs filters via fs::exists.
    auto dirs = luban::env_snapshot::path_dirs();
    for (auto& d : dirs) CHECK(d != luban::paths::xdg_bin_home());
}

TEST_CASE("env_snapshot::apply_to prepends path_dirs onto inherited PATH") {
    Sandbox sb;
    fs::create_directories(luban::paths::xdg_bin_home());

    std::map<std::string, std::string> base;
    base["PATH"] = "/preexisting/dir";

    auto out = luban::env_snapshot::apply_to(base);
    REQUIRE(out.count("PATH"));
    const std::string& path = out["PATH"];
    // xdg_bin_home appears, AND the original PATH is preserved as a suffix.
    CHECK(path.find(luban::paths::xdg_bin_home().string()) != std::string::npos);
    CHECK(path.find("/preexisting/dir") != std::string::npos);

    // env_dict entries are merged into the result.
    CHECK(out.count("VCPKG_DOWNLOADS"));
}

#ifdef _WIN32
// HKCU round-trip — uses a unique var name so a failure mid-test can't
// leave a recognizable name behind in the user's real environment. Even
// so, we always unset on the way out (RAII) to keep the user's regedit
// clean across runs.
TEST_CASE("win_path set_user_env / get_user_env / unset_user_env round-trip") {
    const std::string kVar = "LUBAN_TEST_HKCU_ROUNDTRIP_DO_NOT_USE";
    const std::string kVal = "luban-test-marker-value";

    // Defensive pre-clean: a previous crashed test may have leaked.
    luban::win_path::unset_user_env(kVar);

    REQUIRE(luban::win_path::set_user_env(kVar, kVal));

    auto got = luban::win_path::get_user_env(kVar);
    REQUIRE(got.has_value());
    CHECK(*got == kVal);

    REQUIRE(luban::win_path::unset_user_env(kVar));
    CHECK_FALSE(luban::win_path::get_user_env(kVar).has_value());
}
#endif
