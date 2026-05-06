// `luban migrate` — translate v0.x state into the v1.0 generation
// model. Designed to be safe to run multiple times: detects whether a
// migration has already happened (current.txt exists or generation 1
// already records the migration) and reports "nothing to do" rather
// than re-translating.
//
// What we translate:
//   - <state>/installed.json (v0.x): each ComponentRecord becomes a
//     ToolRecord in generation 1 with from_blueprint = "embedded:cpp-base"
//     (since v0.x setup was the cpp-base equivalent). Shim path stays
//     pointing at the legacy <data>/bin/<alias>.cmd until the user
//     manually re-runs `luban shim` after S7+ retires that path.
//
// What we do NOT translate (deferred):
//   - HKCU PATH: env --user already adds both <data>/bin and ~/.local/bin
//     in v1.0 (S7), so old users get the new dir on next env --user run.
//   - vcpkg / emscripten special-case state: still owned by their
//     post-extract bootstrap; nothing to migrate, generation 1 just
//     records that the components were installed.
//   - User-authored blueprints: there's no v0.x equivalent, so nothing
//     to translate.
//
// Migration is NOT auto-run. v0.x users see the new luban via
// `luban self update` and have to run `luban migrate` themselves.
// This is intentional — auto-migration on every binary launch is a
// classic source of "what just happened to my setup?" surprises, and
// the v0.x setup keeps working without the migration anyway (the
// new commands like blueprint apply just don't see the old state).

#include <cstdio>
#include <fstream>
#include <iostream>

#include "../cli.hpp"
#include "../generation.hpp"
#include "../log.hpp"
#include "../paths.hpp"
#include "../registry.hpp"

namespace luban::commands {

namespace {

namespace fs = std::filesystem;
namespace gen = luban::generation;

constexpr const char* kCppBaseBlueprint = "embedded:cpp-base";

bool already_migrated() {
    auto cur = gen::get_current();
    if (!cur) return false;
    auto g = gen::read(*cur);
    if (!g) return false;
    // We mark migrations by including this blueprint name. If a v1.0
    // user has no v0.x history, gen 1 won't list embedded:cpp-base.
    for (auto& bp : g->applied_blueprints) {
        if (bp == kCppBaseBlueprint) return true;
    }
    return false;
}

}  // namespace

int run_migrate(const cli::ParsedArgs& a) {
    bool dry_run = a.flags.count("dry-run") && a.flags.at("dry-run");

    // Step 1: detect whether there's anything to migrate.
    auto installed = luban::registry::load_installed();
    if (installed.empty()) {
        std::printf("no v0.x state found at %s; nothing to migrate.\n",
                    paths::installed_json_path().string().c_str());
        return 0;
    }

    if (already_migrated()) {
        std::printf("already migrated (current generation includes "
                    "embedded:cpp-base); nothing to do.\n");
        std::printf("  re-run with `luban migrate --force` to re-translate "
                    "(not yet implemented).\n");
        return 0;
    }

    std::printf("v0.x installed.json: %zu components\n", installed.size());
    for (auto& [name, rec] : installed) {
        std::printf("  %s %s (%s)\n", name.c_str(), rec.version.c_str(),
                    rec.architecture.c_str());
    }

    if (dry_run) {
        std::printf("\n--dry-run; not writing generation. Re-run without "
                    "--dry-run to migrate.\n");
        return 0;
    }

    // Step 2: build a Generation. Use the next id (so existing
    // generations from blueprint apply aren't overwritten).
    int next_id = gen::highest_id() + 1;
    gen::Generation g;
    g.schema = 1;
    g.id = next_id;
    g.created_at = gen::now_iso8601();

    // Inherit any prior generation's blueprints/files (won't be any
    // for first-migration users, but defensive in case someone applied
    // a blueprint before running migrate).
    if (auto cur = gen::get_current()) {
        if (auto prev = gen::read(*cur)) {
            g.applied_blueprints = prev->applied_blueprints;
            g.tools = prev->tools;
            g.files = prev->files;
        }
    }

    // Mark embedded:cpp-base applied so already_migrated() returns true
    // on subsequent runs.
    auto& bps = g.applied_blueprints;
    if (std::find(bps.begin(), bps.end(), std::string(kCppBaseBlueprint)) ==
        bps.end()) {
        bps.emplace_back(kCppBaseBlueprint);
    }

    // Translate each component. The shim path stays at <data>/bin until
    // the user re-runs `luban shim` (or `luban env --user` re-points
    // PATH). artifact_id is best-effort — we encode as
    // "<name>-<version>-<arch>-migrated" since we don't have the sha256
    // of the original download handy.
    fs::path legacy_bin = paths::bin_dir();
    for (auto& [name, rec] : installed) {
        for (auto& [alias, _rel] : rec.bins) {
            gen::ToolRecord tr;
            tr.from_blueprint = kCppBaseBlueprint;
            tr.artifact_id =
                rec.name + "-" + rec.version + "-" + rec.architecture + "-migrated";
            // Legacy shim path under <data>/bin/<alias>.cmd. v1.0+
            // shims live in ~/.local/bin/, but we don't rewrite them
            // here — just record where the v0.x ones are.
            tr.shim_path = (legacy_bin / (alias + ".cmd")).string();
            tr.is_external = false;
            g.tools[alias] = std::move(tr);
        }
    }

    if (auto w = gen::write(g); !w) {
        std::cerr << "write generation: " << w.error() << "\n";
        return 1;
    }
    if (auto s = gen::set_current(next_id); !s) {
        std::cerr << "set current: " << s.error() << "\n";
        return 1;
    }

    std::printf("migrated -> generation %d (%zu tool aliases recorded)\n",
                next_id, g.tools.size());
    std::printf("\nnotes:\n");
    std::printf("  - v0.x shims still at %s; new blueprint shims will go to %s.\n",
                legacy_bin.string().c_str(),
                paths::xdg_bin_home().string().c_str());
    std::printf("  - run `luban env --user` to add ~/.local/bin/ to HKCU PATH "
                "(both dirs end up on PATH in transition).\n");
    std::printf("  - run `luban bp apply main/cli-quality` to opt into the "
                "v1.0 personal toolset model.\n");
    return 0;
}

void register_migrate() {
    cli::Subcommand c;
    c.name = "migrate";
    c.help = "migrate v0.x installed.json into the v1.0 generation model";
    c.group = "advanced";
    c.long_help =
        "  `luban migrate` translates v0.x state into a v1.0 generation\n"
        "  snapshot, so existing users upgrading via `luban self update`\n"
        "  see their installed components reflected under `luban bp status`\n"
        "  and friends. Run once after upgrading; rerun is a no-op.\n\n"
        "  --dry-run    Print what would be migrated without writing.";
    c.flags = {"dry-run"};
    c.run = run_migrate;
    cli::register_subcommand(std::move(c));
}

}  // namespace luban::commands
