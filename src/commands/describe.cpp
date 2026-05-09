// `luban describe [--json]` — dump system + (optional) project state.
// IDE / visualization backend; also useful for "what's installed where, exactly?".
//
// Output mode:
//   default: human-readable text (similar to luban doctor but with more detail)
//   --json : single JSON object on stdout, suitable for piping to jq / IDE plugin

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string_view>

#include "../blueprint_lock.hpp"
#include "../cli.hpp"
#include "../describe_state.hpp"
#include "../lib_targets.hpp"
#include "../log.hpp"
#include "../paths.hpp"
#include "../perception.hpp"
#include "../source_registry.hpp"

#include "json.hpp"

namespace luban::commands {

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

void print_text(const json& s) {
    std::cout << "Luban " << s.value("luban_version", "?") << "\n\n";

    std::cout << "── Paths ──\n";
    if (s.contains("paths")) {
        for (auto& [k, v] : s["paths"].items()) {
            std::cout << "  " << k;
            for (size_t i = k.size(); i < 18; ++i) std::cout << ' ';
            std::cout << "  " << v.get<std::string>() << '\n';
        }
    }
    std::cout << '\n';

    // installed_components was the v0.x registry view. v0.5.0+ uses
    // `luban bp list` for the analogous information (per-source bp
    // enumeration).


    if (s.contains("project")) {
        auto& p = s["project"];
        std::cout << "── Project: " << p.value("name", "(unnamed)")
                  << " " << p.value("version", "") << " ──\n";
        std::cout << "  root: " << p["root"].get<std::string>() << '\n';
        if (p.contains("dependencies")) {
            std::cout << "  deps (" << p["dependencies"].size() << "):";
            for (auto& d : p["dependencies"]) {
                std::cout << "  " << d["name"].get<std::string>();
                if (d.contains("version_ge")) std::cout << "@>=" << d["version_ge"].get<std::string>();
            }
            std::cout << '\n';
        }
        if (p.contains("luban_toml")) {
            auto& t = p["luban_toml"];
            std::cout << "  luban.toml: cpp=" << t.value("cpp", 23)
                      << " triplet=" << t.value("triplet", "")
                      << " warnings=" << t.value("warnings", "")
                      << " sanitizers=" << t.value("sanitizers", json::array()).dump() << '\n';
        }
        if (p.contains("builds") && !p["builds"].empty()) {
            std::cout << "  builds:";
            for (auto& b : p["builds"]) {
                std::cout << "  " << b["preset"].get<std::string>()
                          << "(" << (b.value("compile_commands", false) ? "✓" : "—") << ")";
            }
            std::cout << '\n';
        }
    } else {
        std::cout << "(not in a luban project — `cd` into one for project info)\n";
    }
}

// `describe port:<name>` — dump the cmake target mapping for a vcpkg port
// (DESIGN §5: introspection prefix). Reads the in-binary lib_targets table.
int run_describe_port(std::string_view port, bool as_json) {
    auto m = lib_targets::lookup(port);
    if (as_json) {
        json out;
        out["schema"] = 1;
        out["port"] = std::string(port);
        if (m) {
            out["found"] = true;
            out["find_package_name"] = m->find_package_name;
            out["link_targets"] = m->link_targets;
        } else {
            out["found"] = false;
        }
        std::cout << out.dump(2) << '\n';
        return m ? 0 : 1;
    }
    if (!m) {
        std::cout << "port `" << port << "` is not in luban's known table.\n"
                  << "  add a [ports.\"" << port << "\"] block to luban.toml,\n"
                  << "  or wire find_package(<X> CONFIG REQUIRED) by hand.\n";
        return 1;
    }
    std::cout << "port: " << port << '\n';
    std::cout << "  find_package: " << m->find_package_name << '\n';
    std::cout << "  link targets:";
    for (auto& t : m->link_targets) std::cout << " " << t;
    std::cout << '\n';
    return 0;
}

// `describe tool:<name>` — walk every reachable bp .lock and report every
// match. Useful for "where is `cmake` pinned?" introspection. Doesn't
// re-resolve from the network — only reports what the on-disk locks say.
int run_describe_tool(std::string_view tool, bool as_json) {
    namespace sr = luban::source_registry;
    namespace bpl = luban::blueprint_lock;

    // Search roots: user-local + every registered source's blueprints/ dir.
    std::vector<std::pair<std::string, fs::path>> roots;  // {label, dir}
    roots.emplace_back("user-local", paths::config_dir() / "blueprints");
    if (auto entries = sr::read(); entries) {
        for (auto& e : *entries) {
            constexpr std::string_view kFileScheme = "file://";
            fs::path root;
            if (std::string_view(e.url).starts_with(kFileScheme)) {
                std::string_view rest = std::string_view(e.url).substr(kFileScheme.size());
                if (rest.starts_with("/") && rest.size() >= 4 &&
                    std::isalpha(static_cast<unsigned char>(rest[1])) &&
                    rest[2] == ':') {
                    rest.remove_prefix(1);
                }
                root = fs::path(std::string(rest));
            } else {
                root = paths::bp_sources_dir(e.name);
            }
            roots.emplace_back(e.name, root / "blueprints");
        }
    }

    json matches = json::array();
    for (auto& [label, dir] : roots) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        for (auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            if (entry.path().extension() != ".lock") continue;
            auto lock = bpl::read_file(entry.path());
            if (!lock) continue;
            auto it = lock->tools.find(std::string(tool));
            if (it == lock->tools.end()) continue;
            json m;
            m["bp_source"] = label;
            m["blueprint"] = lock->blueprint_name;
            m["lock_path"] = entry.path().string();
            m["version"]   = it->second.version;
            m["source"]    = it->second.source;
            json platforms = json::object();
            for (auto& [tgt, lp] : it->second.platforms) {
                json p;
                p["url"] = lp.url;
                p["sha256"] = lp.sha256;
                p["bin"] = lp.bin;
                p["artifact_id"] = lp.artifact_id;
                platforms[tgt] = p;
            }
            m["platforms"] = platforms;
            matches.push_back(std::move(m));
        }
    }

    if (as_json) {
        json out;
        out["schema"] = 1;
        out["tool"] = std::string(tool);
        out["matches"] = matches;
        std::cout << out.dump(2) << '\n';
        return matches.empty() ? 1 : 0;
    }

    if (matches.empty()) {
        std::cout << "tool `" << tool << "` not found in any reachable bp lock.\n";
        return 1;
    }
    std::cout << "tool: " << tool << "  (" << matches.size() << " match(es))\n";
    for (auto& m : matches) {
        std::cout << "  - " << m["bp_source"].get<std::string>() << "/"
                  << m["blueprint"].get<std::string>()
                  << "  version=" << m["version"].get<std::string>()
                  << "  source=" << m["source"].get<std::string>() << '\n';
        for (auto& [tgt, p] : m["platforms"].items()) {
            std::cout << "      [" << tgt << "] " << p["url"].get<std::string>();
            if (!p["sha256"].get<std::string>().empty()) {
                std::cout << "  sha256=" << p["sha256"].get<std::string>().substr(0, 12) << "…";
            } else {
                std::cout << "  " << log::yellow("no sha256");
            }
            std::cout << '\n';
        }
    }
    return 0;
}

int run_describe(const cli::ParsedArgs& a) {
    auto get_flag = [&](const char* name) {
        auto it = a.flags.find(name);
        return it != a.flags.end() && it->second;
    };
    bool as_json = get_flag("json");
    bool want_host = get_flag("host");

    // Positional `port:<name>` / `tool:<name>` prefixes (DESIGN §5
    // introspection). Bare positional or unrecognised prefix falls
    // through to the default project-state describe.
    if (!a.positional.empty()) {
        std::string_view target = a.positional[0];
        constexpr std::string_view kPort = "port:";
        constexpr std::string_view kTool = "tool:";
        if (target.starts_with(kPort)) {
            return run_describe_port(target.substr(kPort.size()), as_json);
        }
        if (target.starts_with(kTool)) {
            return run_describe_tool(target.substr(kTool.size()), as_json);
        }
        std::cerr << "describe: unrecognised target `" << target
                  << "` (use `port:<name>` or `tool:<name>`)\n";
        return 2;
    }

    // --host is a focused mode: just the perception snapshot, JSON or
    // pretty-printed. Bypasses the project-state discovery so it's fast
    // (sub-millisecond) and useful in any directory.
    if (want_host) {
        auto h = perception::snapshot();
        json doc = perception::to_json(h);
        if (as_json) {
            std::cout << doc.dump(2) << '\n';
        } else {
            std::cout << "host: " << h.os_name << " " << h.os_version
                      << " (" << h.arch << ")\n";
            std::cout << "cpu : " << h.cpu_brand
                      << " — " << h.cpu_cores << " cores / "
                      << h.cpu_threads << " threads\n";
            if (!h.cpu_features.empty()) {
                std::cout << "simd: ";
                for (size_t i = 0; i < h.cpu_features.size(); ++i) {
                    if (i) std::cout << ", ";
                    std::cout << h.cpu_features[i];
                }
                std::cout << "\n";
            }
            if (h.ram_total > 0) {
                double gb = static_cast<double>(h.ram_total) / (1024.0 * 1024.0 * 1024.0);
                std::cout << "ram : " << gb << " GB total\n";
            }
            if (!h.tools_on_path.empty()) {
                std::cout << "tools on PATH: ";
                for (size_t i = 0; i < h.tools_on_path.size(); ++i) {
                    if (i) std::cout << ", ";
                    std::cout << h.tools_on_path[i];
                }
                std::cout << "\n";
            }
        }
        return 0;
    }

    json s = luban::describe_state::build();
    if (as_json) std::cout << s.dump(2) << '\n';
    else print_text(s);
    return 0;
}

}  // namespace

void register_describe() {
    cli::Subcommand c;
    c.name = "describe";
    c.help = "print system + project state (--json for machines)";
    c.group = "advanced";
    c.long_help =
        "  Dump everything luban knows: paths, installed toolchains, bin\n"
        "  aliases, and (when run inside a luban project) vcpkg deps +\n"
        "  luban.toml prefs + build state.\n"
        "\n"
        "  Introspection prefixes (DESIGN §5):\n"
        "    port:<name>   show the cmake target mapping luban knows for\n"
        "                  the vcpkg port (find_package + link targets)\n"
        "    tool:<name>   walk every reachable bp .lock and list every\n"
        "                  pin matching the tool name (version, source,\n"
        "                  per-platform url + sha256)\n"
        "\n"
        "  --host    Skip project state; emit only a host capability snapshot\n"
        "            (OS, CPU, RAM, SIMD features, dev tools on PATH, XDG env).\n"
        "            Cheap (~1 ms); usable from any directory.\n"
        "  --json    Machine-readable JSON output. Combines with --host /\n"
        "            port: / tool:.\n"
        "\n"
        "  Default output is human-readable text. The JSON form has schema=1\n"
        "  and is intended for IDE plugins / AI agents / pipeline scripts.";
    c.flags = {"json", "host"};
    // `target` is optional (bare `luban describe` lists project state).
    // Leave n_positional = 0 (treated as a minimum by cli::dispatch); the
    // optional positional is captured in ParsedArgs.positional and the
    // run handler decides what to do with it (port:/tool: prefix or fall
    // through to project-state describe).
    c.positional_names = {"[target]"};
    c.examples = {
        "luban describe\tHuman-readable summary",
        "luban describe --json\tMachine-readable JSON dump",
        "luban describe --host\tHost capability snapshot only",
        "luban describe port:fmt\tPort → cmake target mapping",
        "luban describe tool:cmake\tWhich bp pins cmake, and to what",
        "luban describe --host --json | jq .cpu_features\tJq filter on host",
    };
    c.run = run_describe;
    cli::register_subcommand(std::move(c));
}

}  // namespace luban::commands
