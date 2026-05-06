// Unit tests for src/generation.cpp.

#include <filesystem>
#include <fstream>

#include "blueprint.hpp"
#include "doctest.h"
#include "generation.hpp"
#include "paths.hpp"

namespace fs = std::filesystem;
namespace bp = luban::blueprint;
namespace gen = luban::generation;

namespace {

struct Sandbox {
    fs::path root;
    Sandbox() {
        root = fs::temp_directory_path() /
               ("luban-gen-test-" + std::to_string(::time(nullptr)) + "-" +
                std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(root);
#ifdef _WIN32
        ::_putenv_s("LUBAN_PREFIX", root.string().c_str());
#else
        ::setenv("LUBAN_PREFIX", root.string().c_str(), 1);
#endif
    }
    ~Sandbox() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

gen::Generation sample_gen(int id) {
    gen::Generation g;
    g.schema = 1;
    g.id = id;
    g.created_at = "2026-05-05T12:00:00Z";
    g.applied_blueprints = {"embedded:cpp-base", "cli-quality"};

    gen::ToolRecord rg;
    rg.from_blueprint = "cli-quality";
    rg.artifact_id = "ripgrep-14.0.3-windows-x64-aabbccdd";
    rg.shim_path = "/home/test/.local/bin/rg";
    rg.is_external = false;
    g.tools["ripgrep"] = rg;

    gen::ToolRecord git;
    git.from_blueprint = "embedded:cpp-base";
    git.is_external = true;
    git.external_path = "C:\\scoop\\shims\\git.exe";
    g.tools["git"] = git;

    gen::FileRecord f;
    f.from_blueprint = "cli-quality";
    f.target_path = "~/.config/bat/config";
    f.content_sha256 = "abc123";
    f.mode = bp::FileMode::Replace;
    g.files["~/.config/bat/config"] = f;

    return g;
}

}  // namespace

TEST_CASE("generations_dir / generation_path / current_path layout") {
    Sandbox sb;
    auto state = luban::paths::state_dir();
    CHECK(gen::generations_dir() == state / "generations");
    CHECK(gen::generation_path(7) == state / "generations" / "7.json");
    CHECK(gen::current_path() == state / "current.txt");
}

TEST_CASE("write + read round-trip") {
    Sandbox sb;
    auto original = sample_gen(3);

    auto w = gen::write(original);
    REQUIRE(w.has_value());
    REQUIRE(fs::exists(gen::generation_path(3)));

    auto loaded = gen::read(3);
    REQUIRE(loaded.has_value());
    CHECK(loaded->id == 3);
    CHECK(loaded->schema == 1);
    CHECK(loaded->created_at == "2026-05-05T12:00:00Z");
    CHECK(loaded->applied_blueprints.size() == 2);

    REQUIRE(loaded->tools.count("ripgrep") == 1);
    auto& rg = loaded->tools["ripgrep"];
    CHECK(rg.artifact_id == "ripgrep-14.0.3-windows-x64-aabbccdd");
    CHECK(rg.shim_path == "/home/test/.local/bin/rg");
    CHECK_FALSE(rg.is_external);

    REQUIRE(loaded->tools.count("git") == 1);
    auto& git = loaded->tools["git"];
    CHECK(git.is_external);
    CHECK(git.external_path == "C:\\scoop\\shims\\git.exe");

    REQUIRE(loaded->files.count("~/.config/bat/config") == 1);
    auto& f = loaded->files["~/.config/bat/config"];
    CHECK(f.content_sha256 == "abc123");
    CHECK(f.mode == bp::FileMode::Replace);
}

TEST_CASE("get_current returns nullopt on fresh install") {
    Sandbox sb;
    CHECK_FALSE(gen::get_current().has_value());
}

TEST_CASE("set_current + get_current round-trip") {
    Sandbox sb;
    auto s = gen::set_current(42);
    REQUIRE(s.has_value());
    auto c = gen::get_current();
    REQUIRE(c.has_value());
    CHECK(*c == 42);
}

TEST_CASE("set_current is atomic (no .tmp left behind)") {
    Sandbox sb;
    REQUIRE(gen::set_current(7).has_value());

    auto tmp = gen::current_path();
    tmp += ".tmp";
    CHECK_FALSE(fs::exists(tmp));
    CHECK(fs::exists(gen::current_path()));
}

TEST_CASE("list_ids returns sorted ids; highest_id matches") {
    Sandbox sb;
    REQUIRE(gen::write(sample_gen(2)).has_value());
    REQUIRE(gen::write(sample_gen(5)).has_value());
    REQUIRE(gen::write(sample_gen(1)).has_value());

    auto ids = gen::list_ids();
    REQUIRE(ids.size() == 3);
    CHECK(ids[0] == 1);
    CHECK(ids[1] == 2);
    CHECK(ids[2] == 5);
    CHECK(gen::highest_id() == 5);
}

TEST_CASE("list_ids skips non-integer / non-.json files in generations dir") {
    Sandbox sb;
    auto dir = gen::generations_dir();
    fs::create_directories(dir);

    REQUIRE(gen::write(sample_gen(1)).has_value());

    // Plant a few decoys.
    {
        std::ofstream(dir / "garbage.txt") << "x";
        std::ofstream(dir / "abc.json") << "{}";
        std::ofstream(dir / "1.json.tmp") << "{}";
    }

    auto ids = gen::list_ids();
    REQUIRE(ids.size() == 1);
    CHECK(ids[0] == 1);
}

TEST_CASE("highest_id returns 0 when no generations exist") {
    Sandbox sb;
    CHECK(gen::highest_id() == 0);
}

TEST_CASE("read returns error for missing generation") {
    Sandbox sb;
    auto r = gen::read(99);
    CHECK_FALSE(r.has_value());
}

TEST_CASE("read rejects unsupported schema") {
    Sandbox sb;
    fs::create_directories(gen::generations_dir());
    {
        std::ofstream out(gen::generation_path(7));
        out << R"({"schema": 999, "id": 7})";
    }
    auto r = gen::read(7);
    CHECK_FALSE(r.has_value());
    CHECK(r.error().find("schema") != std::string::npos);
}

TEST_CASE("now_iso8601 returns a parseable UTC string") {
    auto s = gen::now_iso8601();
    // Format: YYYY-MM-DDTHH:MM:SSZ — exactly 20 chars.
    CHECK(s.size() == 20);
    CHECK(s[10] == 'T');
    CHECK(s.back() == 'Z');
}
