// Unit tests for src/blueprint_reconcile.cpp + the unapply disk-restore
// path in commands/blueprint.cpp's run_unapply (we exercise file_deploy
// + xdg_shim + reconcile, mirroring what unapply does locally).
//
// These tests touch the real filesystem under temp_directory_path() —
// reconcile's whole job is fs interaction, mocking would test the mock.
// LUBAN_PREFIX redirects the four XDG homes plus HOME/USERPROFILE so
// `~/` expansion stays inside the sandbox.

#include <filesystem>
#include <fstream>
#include <sstream>

#include "blueprint.hpp"
#include "blueprint_reconcile.hpp"
#include "doctest.h"
#include "file_deploy.hpp"
#include "generation.hpp"
#include "paths.hpp"
#include "store.hpp"
#include "xdg_shim.hpp"

namespace fs = std::filesystem;
namespace bp = luban::blueprint;
namespace gen = luban::generation;
namespace fd = luban::file_deploy;
namespace recon = luban::blueprint_reconcile;

namespace {

struct Sandbox {
    fs::path root;
    Sandbox() {
        root = fs::temp_directory_path() /
               ("luban-recon-test-" + std::to_string(::time(nullptr)) + "-" +
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

std::string read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// Drive the apply layer's record-keeping by hand. The reconcile module
/// only reads generation files + content store + backups + the artifact
/// store, so we don't need real apply() runs to exercise it.
gen::Generation make_gen(int id, std::vector<std::string> applied) {
    gen::Generation g;
    g.schema = 1;
    g.id = id;
    g.created_at = gen::now_iso8601();
    g.applied_blueprints = std::move(applied);
    return g;
}

/// Plant a fake artifact in the store with a binary at <store>/bin/<exe>.
/// Returns the artifact_id we used. Used by tool-shim tests so the
/// recreate path can find a real exe to symlink/.cmd-shim against.
std::string plant_artifact(const std::string& tool_name, const std::string& bin_filename) {
    std::string artifact_id = tool_name + "-1.0.0-test-deadbeef";
    auto store_dir = luban::store::store_path(artifact_id);
    fs::create_directories(store_dir / "bin");
    {
        std::ofstream out(store_dir / "bin" / bin_filename, std::ios::binary);
        out << "#!/bin/sh\necho fake-" << tool_name << "\n";
    }
    // Marker file so store::is_present() would be true (not strictly
    // required for reconcile, which only needs the path to exist).
    {
        std::ofstream(store_dir / ".store-marker.json") << "{}";
    }
    return artifact_id;
}

}  // namespace

TEST_CASE("reconcile_to no-op when no current generation") {
    Sandbox sb;
    auto r = recon::reconcile_to(0);
    REQUIRE(r.has_value());
    CHECK(r->files_restored == 0);
    CHECK(r->shims_removed == 0);
}

TEST_CASE("reconcile_to no-op when target == current") {
    Sandbox sb;
    auto g = make_gen(1, {"A"});
    REQUIRE(gen::write(g).has_value());
    REQUIRE(gen::set_current(1).has_value());
    auto r = recon::reconcile_to(1);
    REQUIRE(r.has_value());
    CHECK(r->files_restored == 0);
}

TEST_CASE("reconcile_to rejects forward direction") {
    Sandbox sb;
    auto g = make_gen(1, {"A"});
    REQUIRE(gen::write(g).has_value());
    REQUIRE(gen::set_current(1).has_value());
    auto r = recon::reconcile_to(7);
    CHECK_FALSE(r.has_value());
    CHECK(r.error().find("forward") != std::string::npos);
}

TEST_CASE("single-step rollback restores file content from backup") {
    Sandbox sb;

    // gen 1 deployed "OLD" to ~/x.txt (no prior content).
    bp::FileSpec s1;
    s1.target_path = "~/x.txt";
    s1.content = "OLD";
    s1.mode = bp::FileMode::Replace;
    auto d1 = fd::deploy(s1, 1);
    REQUIRE(d1.has_value());

    auto g1 = make_gen(1, {"A"});
    gen::FileRecord f1;
    f1.from_blueprint = "A";
    f1.target_path = "~/x.txt";
    f1.content_sha256 = d1->content_sha256;
    f1.mode = bp::FileMode::Replace;
    if (d1->backup_path) f1.backup_path = d1->backup_path->string();
    g1.files["~/x.txt"] = f1;
    REQUIRE(gen::write(g1).has_value());

    // gen 2 overwrites with "NEW" — backup of "OLD" lands in
    // <state>/backups/2/.
    bp::FileSpec s2 = s1;
    s2.content = "NEW";
    auto d2 = fd::deploy(s2, 2);
    REQUIRE(d2.has_value());
    REQUIRE(d2->backup_path.has_value());

    auto g2 = make_gen(2, {"A"});
    gen::FileRecord f2;
    f2.from_blueprint = "A";
    f2.target_path = "~/x.txt";
    f2.content_sha256 = d2->content_sha256;
    f2.mode = bp::FileMode::Replace;
    f2.backup_path = d2->backup_path->string();
    g2.files["~/x.txt"] = f2;
    REQUIRE(gen::write(g2).has_value());
    REQUIRE(gen::set_current(2).has_value());

    CHECK(read_all(luban::paths::home() / "x.txt") == "NEW");

    auto r = recon::reconcile_to(1);
    REQUIRE(r.has_value());
    CHECK(r->files_restored == 1);
    CHECK(read_all(luban::paths::home() / "x.txt") == "OLD");
}

TEST_CASE("single-step rollback removes net-new file") {
    Sandbox sb;

    // gen 1 has nothing.
    auto g1 = make_gen(1, {});
    REQUIRE(gen::write(g1).has_value());

    // gen 2 deployed ~/b.txt fresh (no backup).
    bp::FileSpec s2;
    s2.target_path = "~/b.txt";
    s2.content = "FRESH";
    s2.mode = bp::FileMode::Replace;
    auto d2 = fd::deploy(s2, 2);
    REQUIRE(d2.has_value());
    REQUIRE_FALSE(d2->backup_path.has_value());

    auto g2 = make_gen(2, {"B"});
    gen::FileRecord f2;
    f2.from_blueprint = "B";
    f2.target_path = "~/b.txt";
    f2.content_sha256 = d2->content_sha256;
    f2.mode = bp::FileMode::Replace;
    g2.files["~/b.txt"] = f2;
    REQUIRE(gen::write(g2).has_value());
    REQUIRE(gen::set_current(2).has_value());

    CHECK(fs::exists(luban::paths::home() / "b.txt"));

    auto r = recon::reconcile_to(1);
    REQUIRE(r.has_value());
    CHECK_FALSE(fs::exists(luban::paths::home() / "b.txt"));
}

TEST_CASE("rollback removes shim of tool added in last generation") {
    Sandbox sb;

    // gen 1: no tools.
    auto g1 = make_gen(1, {});
    REQUIRE(gen::write(g1).has_value());

    // gen 2: tool "fake" with shim ~/.local/bin/fake.cmd (Win) or fake (POSIX).
    auto artifact_id = plant_artifact("fake", "fake.exe");
    auto store_dir = luban::store::store_path(artifact_id);
    auto exe = store_dir / "bin" / "fake.exe";

    auto shim = luban::xdg_shim::write_cmd_shim("fake", exe);
    REQUIRE(shim.has_value());
    REQUIRE(fs::exists(*shim));

    auto g2 = make_gen(2, {"T"});
    gen::ToolRecord tr;
    tr.from_blueprint = "T";
    tr.artifact_id = artifact_id;
    tr.shim_path = shim->string();
    tr.bin_path_rel = "bin/fake.exe";
    g2.tools["fake"] = tr;
    REQUIRE(gen::write(g2).has_value());
    REQUIRE(gen::set_current(2).has_value());

    auto r = recon::reconcile_to(1);
    REQUIRE(r.has_value());
    CHECK(r->shims_removed == 1);
    CHECK_FALSE(fs::exists(*shim));
}

TEST_CASE("multi-step rollback chains undo across generations") {
    Sandbox sb;

    // gen 1 deploys ~/a.txt = "A1" (no backup).
    bp::FileSpec sa;
    sa.target_path = "~/a.txt";
    sa.content = "A1";
    sa.mode = bp::FileMode::Replace;
    auto da = fd::deploy(sa, 1);
    REQUIRE(da.has_value());

    auto g1 = make_gen(1, {"A"});
    gen::FileRecord fa;
    fa.from_blueprint = "A";
    fa.target_path = "~/a.txt";
    fa.content_sha256 = da->content_sha256;
    fa.mode = bp::FileMode::Replace;
    g1.files["~/a.txt"] = fa;
    REQUIRE(gen::write(g1).has_value());

    // gen 2 adds ~/b.txt = "B1" on top.
    bp::FileSpec sb_;
    sb_.target_path = "~/b.txt";
    sb_.content = "B1";
    sb_.mode = bp::FileMode::Replace;
    auto db = fd::deploy(sb_, 2);
    REQUIRE(db.has_value());

    auto g2 = make_gen(2, {"A", "B"});
    g2.files = g1.files;
    gen::FileRecord fb;
    fb.from_blueprint = "B";
    fb.target_path = "~/b.txt";
    fb.content_sha256 = db->content_sha256;
    fb.mode = bp::FileMode::Replace;
    g2.files["~/b.txt"] = fb;
    REQUIRE(gen::write(g2).has_value());

    // gen 3 adds ~/c.txt = "C1" on top.
    bp::FileSpec sc;
    sc.target_path = "~/c.txt";
    sc.content = "C1";
    sc.mode = bp::FileMode::Replace;
    auto dc = fd::deploy(sc, 3);
    REQUIRE(dc.has_value());

    auto g3 = make_gen(3, {"A", "B", "C"});
    g3.files = g2.files;
    gen::FileRecord fc;
    fc.from_blueprint = "C";
    fc.target_path = "~/c.txt";
    fc.content_sha256 = dc->content_sha256;
    fc.mode = bp::FileMode::Replace;
    g3.files["~/c.txt"] = fc;
    REQUIRE(gen::write(g3).has_value());
    REQUIRE(gen::set_current(3).has_value());

    REQUIRE(fs::exists(luban::paths::home() / "a.txt"));
    REQUIRE(fs::exists(luban::paths::home() / "b.txt"));
    REQUIRE(fs::exists(luban::paths::home() / "c.txt"));

    // Rollback all the way to gen 1.
    auto r = recon::reconcile_to(1);
    REQUIRE(r.has_value());
    CHECK(read_all(luban::paths::home() / "a.txt") == "A1");
    CHECK_FALSE(fs::exists(luban::paths::home() / "b.txt"));
    CHECK_FALSE(fs::exists(luban::paths::home() / "c.txt"));
}

TEST_CASE("rollback recreates file dropped between target and current via content store") {
    Sandbox sb;

    // gen 1: deploy ~/x.txt = "X1". Content store gets the snapshot.
    bp::FileSpec s1;
    s1.target_path = "~/x.txt";
    s1.content = "X1";
    s1.mode = bp::FileMode::Replace;
    auto d1 = fd::deploy(s1, 1);
    REQUIRE(d1.has_value());

    auto g1 = make_gen(1, {"A"});
    gen::FileRecord f1;
    f1.from_blueprint = "A";
    f1.target_path = "~/x.txt";
    f1.content_sha256 = d1->content_sha256;
    f1.mode = bp::FileMode::Replace;
    g1.files["~/x.txt"] = f1;
    REQUIRE(gen::write(g1).has_value());

    // gen 2: simulate an unapply of A — same disk effect (file removed)
    // and gen 2 has no record for ~/x.txt.
    fs::remove(luban::paths::home() / "x.txt");
    auto g2 = make_gen(2, {});
    REQUIRE(gen::write(g2).has_value());
    REQUIRE(gen::set_current(2).has_value());

    REQUIRE_FALSE(fs::exists(luban::paths::home() / "x.txt"));
    // Content store still has the snapshot from gen 1's deploy.
    REQUIRE(fs::exists(fd::content_store_path(d1->content_sha256)));

    auto r = recon::reconcile_to(1);
    REQUIRE(r.has_value());
    CHECK(r->files_recreated == 1);
    CHECK(read_all(luban::paths::home() / "x.txt") == "X1");
}

TEST_CASE("legacy tool record without bin_path_rel surfaces warning") {
    Sandbox sb;

    // gen 1: tool was applied by an older luban that didn't track
    // bin_path_rel. We simulate this by writing a generation file with
    // the field absent.
    auto artifact_id = plant_artifact("legacy", "legacy.exe");

    auto g1 = make_gen(1, {});
    REQUIRE(gen::write(g1).has_value());

    auto g2 = make_gen(2, {"L"});
    gen::ToolRecord tr;
    tr.from_blueprint = "L";
    tr.artifact_id = artifact_id;
    tr.shim_path = "/nonexistent/bin/legacy";
    // Deliberately leave bin_path_rel empty.
    g2.tools["legacy"] = tr;
    REQUIRE(gen::write(g2).has_value());

    // gen 3 unapplied L, no legacy entry.
    auto g3 = make_gen(3, {});
    REQUIRE(gen::write(g3).has_value());
    REQUIRE(gen::set_current(3).has_value());

    // Rolling back to gen 2 needs to recreate legacy's shim — but
    // bin_path_rel is missing. Expect a warning.
    auto r = recon::reconcile_to(2);
    REQUIRE(r.has_value());
    bool found = false;
    for (auto& w : r->warnings) {
        if (w.find("bin_path_rel") != std::string::npos &&
            w.find("legacy") != std::string::npos) {
            found = true;
            break;
        }
    }
    CHECK(found);
}
