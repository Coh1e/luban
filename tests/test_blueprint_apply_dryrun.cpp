// Unit tests for `blueprint_apply::apply` in dry_run mode (DESIGN §10.1).
//
// Goal: prove that with opts.dry_run = true the orchestrator
//   - does NOT write the target files described by `[file]` blocks
//   - does NOT touch <state>/luban/applied.txt (mark_applied skipped)
//   - does NOT touch <state>/luban/owned-shims.txt (no shims recorded)
// and that the same blueprint with dry_run = false DOES deploy the file
// and DOES land in applied.txt — a parity / sanity baseline.
//
// We deliberately avoid tools (no network) and configs (no renderer
// registry needed) — a single FileSpec is enough to exercise every
// dry-run guard in apply() that doesn't require fetching from S3.

#include <filesystem>
#include <fstream>

#include "applied_db.hpp"
#include "blueprint.hpp"
#include "blueprint_apply.hpp"
#include "blueprint_lock.hpp"
#include "doctest.h"
#include "paths.hpp"

namespace fs = std::filesystem;
namespace bp = luban::blueprint;
namespace bpa = luban::blueprint_apply;
namespace bpl = luban::blueprint_lock;

namespace {

// Mirror of test_file_deploy.cpp's Sandbox: redirect LUBAN_PREFIX +
// HOME so paths::* land inside a temp tree per test.
struct Sandbox {
    fs::path root;
    Sandbox() {
        root = fs::temp_directory_path() /
               ("luban-bpa-dryrun-" + std::to_string(::time(nullptr)) + "-" +
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

bp::BlueprintSpec make_one_file_spec() {
    bp::BlueprintSpec spec;
    spec.schema = 1;
    spec.name = "dryrun-fixture";
    spec.description = "single-file blueprint for dry-run tests";

    bp::FileSpec f;
    f.target_path = "~/dryrun-target.txt";
    f.content = "this should not appear on disk in dry-run mode\n";
    f.mode = bp::FileMode::Replace;
    spec.files.push_back(std::move(f));
    return spec;
}

}  // namespace

TEST_CASE("apply dry_run skips file deploy + applied.txt") {
    Sandbox sb;
    auto spec = make_one_file_spec();
    bpl::BlueprintLock lock;
    lock.blueprint_name = spec.name;

    bpa::ApplyOptions opts;
    opts.dry_run = true;

    auto r = bpa::apply(spec, lock, opts);
    REQUIRE(r.has_value());
    // files_deployed counts dry-run "would" entries — preserves observability.
    CHECK(r->files_deployed == 1);
    CHECK(r->tools_fetched == 0);
    CHECK(r->tools_external == 0);

    // The whole point: nothing landed.
    CHECK_FALSE(fs::exists(luban::paths::home() / "dryrun-target.txt"));
    CHECK_FALSE(luban::applied_db::is_applied(spec.name));
    CHECK(luban::applied_db::list_owned_shims().empty());
}

TEST_CASE("apply without dry_run deploys + marks applied (parity baseline)") {
    Sandbox sb;
    auto spec = make_one_file_spec();
    bpl::BlueprintLock lock;
    lock.blueprint_name = spec.name;

    bpa::ApplyOptions opts;
    opts.dry_run = false;

    auto r = bpa::apply(spec, lock, opts);
    REQUIRE(r.has_value());
    CHECK(r->files_deployed == 1);

    auto target = luban::paths::home() / "dryrun-target.txt";
    REQUIRE(fs::exists(target));
    std::ifstream in(target, std::ios::binary);
    std::string body((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    CHECK(body == "this should not appear on disk in dry-run mode\n");

    CHECK(luban::applied_db::is_applied(spec.name));
}

TEST_CASE("apply dry_run is idempotent — second run still touches nothing") {
    Sandbox sb;
    auto spec = make_one_file_spec();
    bpl::BlueprintLock lock;
    lock.blueprint_name = spec.name;

    bpa::ApplyOptions opts;
    opts.dry_run = true;

    REQUIRE(bpa::apply(spec, lock, opts).has_value());
    REQUIRE(bpa::apply(spec, lock, opts).has_value());

    CHECK_FALSE(fs::exists(luban::paths::home() / "dryrun-target.txt"));
    CHECK_FALSE(luban::applied_db::is_applied(spec.name));
}
