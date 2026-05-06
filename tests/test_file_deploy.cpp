// Unit tests for src/file_deploy.cpp.
//
// These DO touch the real filesystem (under temp_directory_path()) since
// the module's whole job is fs interaction; mocking would test the mock,
// not the code. We clean up after every case.

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "blueprint.hpp"
#include "doctest.h"
#include "file_deploy.hpp"
#include "paths.hpp"

namespace fs = std::filesystem;
namespace bp = luban::blueprint;
namespace fd = luban::file_deploy;

namespace {

/// Read whole file as a string.
std::string read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// Per-test sandbox: redirect $LUBAN_PREFIX so paths::home() / state_dir()
/// stay inside the temp area and we don't pollute the user's real home.
struct Sandbox {
    fs::path root;
    Sandbox() {
        root = fs::temp_directory_path() /
               ("luban-fd-test-" + std::to_string(::time(nullptr)) + "-" +
                std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(root);
        // LUBAN_PREFIX redirects all four XDG homes under <prefix>/<role>.
        // paths.cpp respects it (see paths::data_dir() and friends).
#ifdef _WIN32
        ::_putenv_s("LUBAN_PREFIX", root.string().c_str());
        // For ~/ expansion we also need HOME / USERPROFILE to land inside
        // the sandbox so stray writes don't escape.
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

}  // namespace

TEST_CASE("expand_home handles ~ and ~/foo") {
    Sandbox sb;
    auto h = luban::paths::home();
    CHECK(fd::expand_home("~") == h);
    CHECK(fd::expand_home("~/foo") == h / "foo");
    CHECK(fd::expand_home("~/.config/bat/config") == h / ".config/bat/config");
    // Backslash variant on Windows-style paths.
    CHECK(fd::expand_home("~\\foo") == h / "foo");
    // Non-tilde paths pass through.
    CHECK(fd::expand_home("/abs/path") == fs::path("/abs/path"));
    CHECK(fd::expand_home("relative/path") == fs::path("relative/path"));
}

TEST_CASE("deploy replace mode writes content to target") {
    Sandbox sb;
    bp::FileSpec spec;
    spec.target_path = "~/myfile.txt";
    spec.content = "hello, world\n";
    spec.mode = bp::FileMode::Replace;

    auto r = fd::deploy(spec, 1);
    REQUIRE(r.has_value());
    CHECK(r->target_path == luban::paths::home() / "myfile.txt");
    CHECK(r->mode == bp::FileMode::Replace);
    CHECK_FALSE(r->backup_path.has_value());  // no preexisting file
    CHECK(read_all(r->target_path) == "hello, world\n");
    CHECK(r->content_sha256.size() == 64);  // sha256 hex is 64 chars
}

TEST_CASE("deploy replace backs up preexisting target") {
    Sandbox sb;
    auto target = luban::paths::home() / "preexisting.txt";
    fs::create_directories(target.parent_path());
    {
        std::ofstream out(target);
        out << "ORIGINAL CONTENT";
    }

    bp::FileSpec spec;
    spec.target_path = "~/preexisting.txt";
    spec.content = "NEW CONTENT";
    spec.mode = bp::FileMode::Replace;

    auto r = fd::deploy(spec, 5);
    REQUIRE(r.has_value());
    REQUIRE(r->backup_path.has_value());
    CHECK(read_all(r->target_path) == "NEW CONTENT");
    CHECK(read_all(*r->backup_path) == "ORIGINAL CONTENT");

    // Backup lives under <state>/backups/<gen>/.
    auto expected_backup_dir = luban::paths::state_dir() / "backups" / "5";
    CHECK(r->backup_path->parent_path() == expected_backup_dir);
}

TEST_CASE("deploy creates parent directory if missing") {
    Sandbox sb;
    bp::FileSpec spec;
    spec.target_path = "~/nested/deeper/file.toml";
    spec.content = "x = 1\n";
    spec.mode = bp::FileMode::Replace;

    auto r = fd::deploy(spec, 1);
    REQUIRE(r.has_value());
    CHECK(fs::exists(r->target_path));
    CHECK(fs::is_directory(r->target_path.parent_path()));
}

TEST_CASE("deploy drop-in mode does NOT back up (luban owns drop-in subdir)") {
    Sandbox sb;
    auto target = luban::paths::home() / ".gitconfig.d" / "aliases";
    fs::create_directories(target.parent_path());
    {
        std::ofstream out(target);
        out << "PREVIOUS";
    }

    bp::FileSpec spec;
    spec.target_path = "~/.gitconfig.d/aliases";
    spec.content = "[alias]\n    co = checkout\n";
    spec.mode = bp::FileMode::DropIn;

    auto r = fd::deploy(spec, 1);
    REQUIRE(r.has_value());
    CHECK(r->mode == bp::FileMode::DropIn);
    // Drop-in mode never backs up — luban owns this whole subdir, the
    // previous version was also luban-written.
    CHECK_FALSE(r->backup_path.has_value());
    CHECK(read_all(r->target_path).find("checkout") != std::string::npos);
}

TEST_CASE("restore puts original back when backup exists") {
    Sandbox sb;
    auto target = luban::paths::home() / "x.txt";
    fs::create_directories(target.parent_path());
    {
        std::ofstream out(target);
        out << "ORIGINAL";
    }

    bp::FileSpec spec;
    spec.target_path = "~/x.txt";
    spec.content = "NEW";
    spec.mode = bp::FileMode::Replace;

    auto deployed = fd::deploy(spec, 7);
    REQUIRE(deployed.has_value());
    CHECK(read_all(target) == "NEW");

    auto restored = fd::restore(*deployed);
    REQUIRE(restored.has_value());
    CHECK(read_all(target) == "ORIGINAL");
}

TEST_CASE("restore removes target when no backup (no original existed)") {
    Sandbox sb;
    bp::FileSpec spec;
    spec.target_path = "~/fresh.txt";
    spec.content = "NEW";
    spec.mode = bp::FileMode::Replace;

    auto deployed = fd::deploy(spec, 1);
    REQUIRE(deployed.has_value());
    REQUIRE(fs::exists(deployed->target_path));
    REQUIRE_FALSE(deployed->backup_path.has_value());

    auto restored = fd::restore(*deployed);
    REQUIRE(restored.has_value());
    CHECK_FALSE(fs::exists(deployed->target_path));
}
