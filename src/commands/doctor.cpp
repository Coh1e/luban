// `luban doctor` — health report.
//
// Modes:
//   - default      : pretty text output, always exit 0 (for human + bug-report use)
//   - --strict     : same text, but exit 1 if any check failed (CI gate)
//   - --json       : machine-readable JSON, schema=1; for IDE plugins / agents
//
// Checks (each yields a pass/fail):
//   1. 4 canonical homes (data / cache / state / config) — exist on disk?
//   2. Subdirectories (toolchains, bin, registry/overlay, downloads, ...) — exist?
//   3. installed.json present and non-empty?
//   4. Each expected tool (cmake / ninja / clang / git / vcpkg / clangd / ...) on PATH?
//
// Failure semantics: a missing dir auto-created by ensure_dirs() is still a fail
// from doctor's POV, since the user just observed the world without auto-fix.
// The path layer in paths::ensure_dirs() is what creates them; doctor doesn't
// silently fix.

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "json.hpp"

#include "../blueprint_lock.hpp"
#include "../cli.hpp"
#include "../log.hpp"
#include "../applied_db.hpp"
#include "../path_search.hpp"
#include "../paths.hpp"
#include "../source_registry.hpp"
#include "../tool_list.hpp"

namespace luban::commands {

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

// Single check outcome — used by both renderers (text + JSON) so they
// stay in sync without re-running the work.
struct CheckRow {
    std::string label;        // human label (printed) + key (in JSON)
    std::string detail;       // path / version / found-at — empty allowed
    bool ok = false;
};

struct Report {
    std::vector<CheckRow> homes;
    std::vector<CheckRow> subdirs;
    std::vector<CheckRow> components;     // detail = "<version>"; ok=false if empty
    std::vector<CheckRow> tools;
    std::vector<CheckRow> trust;          // DESIGN §8: source officiality + TOFU
    bool volume_warning = false;          // data/cache on different volumes

    // True iff every recorded check passed. Drives --strict exit code.
    bool all_ok() const {
        for (auto* list : {&homes, &subdirs, &components, &tools, &trust}) {
            for (auto& r : *list) if (!r.ok) return false;
        }
        return true;
    }
};

Report build_report() {
    Report r;
    std::error_code ec;

    // 1. Homes (4 canonical dirs).
    {
        struct Pair { std::string role; fs::path p; };
        std::array<Pair, 4> rows = {{
            {"data",   paths::data_dir()},
            {"cache",  paths::cache_dir()},
            {"state",  paths::state_dir()},
            {"config", paths::config_dir()},
        }};
        for (auto& row : rows) {
            r.homes.push_back({row.role, row.p.string(), fs::exists(row.p, ec)});
        }
        r.volume_warning = !paths::same_volume(paths::data_dir(), paths::cache_dir());
    }

    // 2. Sub-dirs (everything in all_dirs minus the 4 homes).
    {
        for (auto& [label, p] : paths::all_dirs()) {
            if (label == "data" || label == "cache" || label == "state" || label == "config") continue;
            r.subdirs.push_back({label, p.string(), fs::exists(p, ec)});
        }
    }

    // 3. Applied blueprints (v0.5.0+ replaces v0.x installed.json).
    {
        std::ifstream in(luban::applied_db::applied_path());
        if (!in) {
            r.components.push_back({"<applied>", "no bps applied — try `luban bp apply main/bootstrap`", false});
        } else {
            std::string line;
            int count = 0;
            while (std::getline(in, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
                if (line.empty()) continue;
                r.components.push_back({line, "applied", true});
                ++count;
            }
            if (count == 0) {
                r.components.push_back({"<applied>", "applied.txt is empty", false});
            }
        }
    }

    // 4. Tools on PATH.
    {
        for (auto tool : tool_list::kCoreTools) {
            std::string found = path_search::on_path_str(tool);
            r.tools.push_back({std::string(tool), found, !found.empty()});
        }
    }

    // 5. Trust checks (DESIGN §8). Two surfaces here:
    //    a) Per registered source: official-allowlist membership
    //    b) Per applied bp's .lock: tools with empty sha256 = TOFU
    //
    // external_skip auditing is owned by the apply-time trust summary
    // (rendered in blueprint_apply.cpp) — it has the full bp spec in
    // hand and can report probe target + PATH hit per tool. Re-doing
    // the same in doctor would require dragging lua_engine + a bp
    // re-parse into doctor's link surface for a redundant signal.
    // DESIGN §8 codifies this split.
    {
        auto entries = source_registry::read();
        std::vector<fs::path> bp_lock_search_roots;
        bp_lock_search_roots.push_back(paths::config_dir() / "blueprints");
        if (entries) {
            for (auto& e : *entries) {
                std::string label = "source: " + e.name;
                if (e.official) {
                    r.trust.push_back({label, e.url + " (official)", true});
                } else {
                    r.trust.push_back({label, e.url + " — NON-OFFICIAL", false});
                }
                // Mirror commands/blueprint.cpp::source_root_for: file://
                // sources live-link to the user's repo; remote sources land
                // at paths::bp_sources_dir(name).
                constexpr std::string_view kFileScheme = "file://";
                fs::path root;
                if (std::string_view(e.url).starts_with(kFileScheme)) {
                    std::string_view rest = std::string_view(e.url).substr(kFileScheme.size());
                    if (rest.starts_with("/") && rest.size() >= 4 &&
                        std::isalpha(static_cast<unsigned char>(rest[1])) && rest[2] == ':') {
                        rest.remove_prefix(1);
                    }
                    root = fs::path(std::string(rest));
                } else {
                    root = paths::bp_sources_dir(e.name);
                }
                bp_lock_search_roots.push_back(root / "blueprints");
            }
        }

        // Walk *.lock files under every search root, collect TOFU tools.
        for (auto& root : bp_lock_search_roots) {
            std::error_code dec;
            if (!fs::is_directory(root, dec)) continue;
            for (auto& entry : fs::directory_iterator(root, dec)) {
                if (!entry.is_regular_file(dec)) continue;
                auto p = entry.path();
                if (p.extension() != ".lock") continue;
                auto lock = blueprint_lock::read_file(p);
                if (!lock) continue;
                for (auto& [tool_name, lt] : lock->tools) {
                    for (auto& [tgt, lp] : lt.platforms) {
                        if (lp.sha256.empty()) {
                            r.trust.push_back({
                                "TOFU tool: " + lock->blueprint_name + "/" + tool_name,
                                "no sha256 pin in lock (target=" + tgt + ", url=" +
                                    lp.url + ")",
                                false});
                        }
                    }
                }
            }
        }

        // Friendly empty-state row so an all-clean machine doesn't show a
        // suspicious bare section. The marker is a green check; the row
        // counts as ok=true so --strict isn't tripped.
        if (r.trust.empty()) {
            r.trust.push_back({"(none)", "no registered sources or locks scanned", true});
        }
    }

    return r;
}

// ---- Text renderer ----------------------------------------------------------

void println(const std::string& s) {
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fputc('\n', stdout);
}

std::string pad(std::string_view s, size_t width) {
    if (s.size() >= width) return std::string(s);
    std::string out(s);
    out.append(width - s.size(), ' ');
    return out;
}

std::string marker(bool ok) {
    return ok ? log::green("\xe2\x9c\x93") : log::dim("\xc2\xb7");
}

void render_text(const Report& r) {
    log::step("Canonical homes");
    size_t home_w = 0;
    for (auto& row : r.homes) home_w = std::max(home_w, row.label.size());
    for (auto& row : r.homes) {
        println("  " + marker(row.ok) + " " + pad(row.label, home_w) + "  " + row.detail);
    }
    if (r.volume_warning) {
        log::warn("data and cache are on different volumes \xe2\x80\x94 hardlink "
                  "deduplication will fall back to copy.");
    }

    std::cout << '\n';
    log::step("Sub-directories");
    for (auto& row : r.subdirs) {
        println("  " + marker(row.ok) + " " + pad(row.label, 18) + "  " + row.detail);
    }

    std::cout << '\n';
    log::step("Installed components");
    if (r.components.size() == 1 && r.components[0].label == "<registry>") {
        println("  (" + r.components[0].detail + ")");
    } else {
        for (auto& row : r.components) {
            std::string m = row.ok ? log::green("\xe2\x9c\x93") : log::red("\xe2\x9c\x97");
            println("  " + m + " " + pad(row.label, 28) + " " + row.detail);
        }
    }

    std::cout << '\n';
    log::step("Tools on PATH");
    for (auto& row : r.tools) {
        std::string loc = row.ok ? row.detail : log::dim("(not found)");
        println("  " + marker(row.ok) + " " + pad(row.label, 14) + " " + loc);
    }

    std::cout << '\n';
    log::step("Trust (DESIGN §8)");
    size_t trust_w = 0;
    for (auto& row : r.trust) trust_w = std::max(trust_w, row.label.size());
    if (trust_w > 36) trust_w = 36;
    for (auto& row : r.trust) {
        std::string m = row.ok ? log::green("\xe2\x9c\x93") : log::red("!");
        println("  " + m + " " + pad(row.label, trust_w) + "  " + row.detail);
    }
}

// ---- JSON renderer ----------------------------------------------------------

json row_to_json(const CheckRow& r) {
    return json::object({{"label", r.label}, {"detail", r.detail}, {"ok", r.ok}});
}

json rows_to_json(const std::vector<CheckRow>& rows) {
    json arr = json::array();
    for (auto& r : rows) arr.push_back(row_to_json(r));
    return arr;
}

void render_json(const Report& r) {
    json doc = json::object({
        {"schema", 1},
        {"all_ok", r.all_ok()},
        {"volume_warning", r.volume_warning},
        {"homes", rows_to_json(r.homes)},
        {"subdirs", rows_to_json(r.subdirs)},
        {"components", rows_to_json(r.components)},
        {"tools", rows_to_json(r.tools)},
        {"trust", rows_to_json(r.trust)},
    });
    std::cout << doc.dump(2) << '\n';
}

int run_doctor(const cli::ParsedArgs& args) {
    bool want_json   = args.flags.count("json")   && args.flags.at("json");
    bool want_strict = args.flags.count("strict") && args.flags.at("strict");

    Report r = build_report();

    if (want_json) {
        render_json(r);
    } else {
        render_text(r);
    }

    return (want_strict && !r.all_ok()) ? 1 : 0;
}

}  // namespace

void register_doctor() {
    cli::Subcommand c;
    c.name = "doctor";
    c.help = "report toolchain health";
    c.group = "advanced";
    c.long_help =
        "  Print luban's view of the world:\n"
        "    - the 4 canonical homes (data/cache/state/config)\n"
        "    - whether each subdirectory exists\n"
        "    - which components are recorded in installed.json\n"
        "    - whether the expected tools (clang/cmake/ninja/...) are on PATH\n"
        "\n"
        "  --strict   exit non-zero if any check failed (for CI gates)\n"
        "  --json     emit a machine-readable JSON report (schema=1; for IDE\n"
        "             plugins, AI agents). Implies non-pretty stdout.";
    c.flags = {"strict", "json"};
    c.examples = {
        "luban doctor\tHuman-readable report",
        "luban doctor --strict\tExit 1 if anything is broken",
        "luban doctor --json | jq .all_ok\tCheck programmatically",
    };
    c.run = run_doctor;
    cli::register_subcommand(std::move(c));
}

}  // namespace luban::commands
