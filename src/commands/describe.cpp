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

#include "../cli.hpp"
#include "../log.hpp"
#include "../paths.hpp"
#include "../registry.hpp"
#include "../vcpkg_manifest.hpp"
#include "../luban_toml.hpp"

#include "json.hpp"

namespace luban::commands {

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

// 找项目根：cwd 向上一直到根，第一个含 luban.cmake 或 vcpkg.json 的目录。
fs::path find_project_root() {
    std::error_code ec;
    fs::path d = fs::current_path(ec);
    while (!d.empty()) {
        if (fs::exists(d / "luban.cmake", ec) || fs::exists(d / "vcpkg.json", ec)) return d;
        fs::path parent = d.parent_path();
        if (parent == d) break;
        d = parent;
    }
    return {};
}

json build_state() {
    json out;

    // ---- system tier ----
    out["luban_version"] = "0.1.0";

    json paths_obj;
    for (auto& [name, p] : paths::all_dirs()) paths_obj[name] = p.string();
    out["paths"] = paths_obj;

    json comps = json::array();
    for (auto& [name, rec] : registry::load_installed()) {
        json bins_arr = json::array();
        for (auto& [alias, rel] : rec.bins) {
            json b;
            b["alias"] = alias;
            b["relative_path"] = rel;
            b["absolute_path"] = (paths::toolchain_dir(rec.toolchain_dir) / rel).string();
            bins_arr.push_back(b);
        }
        json c;
        c["name"] = name;
        c["version"] = rec.version;
        c["source"] = rec.source;
        c["url"] = rec.url;
        c["hash"] = rec.hash_spec;
        c["toolchain_dir"] = (paths::toolchains_dir() / rec.toolchain_dir).string();
        c["architecture"] = rec.architecture;
        c["installed_at"] = rec.installed_at;
        c["bins"] = bins_arr;
        comps.push_back(c);
    }
    out["installed_components"] = comps;

    // ---- project tier (if cwd is in a luban project) ----
    fs::path proj = find_project_root();
    if (!proj.empty()) {
        json p;
        p["root"] = proj.string();

        // vcpkg.json deps
        std::error_code ec;
        if (fs::exists(proj / "vcpkg.json", ec)) {
            auto m = vcpkg_manifest::load(proj / "vcpkg.json");
            json deps = json::array();
            for (auto& d : m.dependencies) {
                json dj;
                dj["name"] = d.name;
                if (d.version_ge) dj["version_ge"] = *d.version_ge;
                deps.push_back(dj);
            }
            p["name"] = m.name;
            p["version"] = m.version;
            p["dependencies"] = deps;
        }

        // luban.toml prefs
        if (fs::exists(proj / "luban.toml", ec)) {
            auto t = luban_toml::load(proj / "luban.toml");
            json toml_obj;
            toml_obj["cpp"] = t.project.cpp;
            toml_obj["triplet"] = t.project.triplet;
            toml_obj["default_preset"] = t.project.default_preset;
            const char* warn_str = "normal";
            switch (t.scaffold.warnings) {
                case luban_toml::WarningLevel::Off: warn_str = "off"; break;
                case luban_toml::WarningLevel::Strict: warn_str = "strict"; break;
                default: break;
            }
            toml_obj["warnings"] = warn_str;
            toml_obj["sanitizers"] = t.scaffold.sanitizers;
            p["luban_toml"] = toml_obj;
        }

        // build state
        json builds = json::array();
        if (fs::exists(proj / "build", ec)) {
            for (auto& entry : fs::directory_iterator(proj / "build", ec)) {
                if (!entry.is_directory()) continue;
                json b;
                b["preset"] = entry.path().filename().string();
                b["dir"] = entry.path().string();
                b["compile_commands"] = fs::exists(entry.path() / "compile_commands.json", ec);
                builds.push_back(b);
            }
        }
        p["builds"] = builds;
        p["compile_commands_root"] = (fs::exists(proj / "compile_commands.json", ec))
                                         ? (proj / "compile_commands.json").string()
                                         : "";
        out["project"] = p;
    }

    return out;
}

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

    std::cout << "── Installed components (" << s["installed_components"].size() << ") ──\n";
    for (auto& c : s["installed_components"]) {
        std::cout << "  " << c["name"].get<std::string>()
                  << " " << c["version"].get<std::string>()
                  << "  →  " << c["toolchain_dir"].get<std::string>() << '\n';
        std::cout << "      "
                  << c["bins"].size() << " alias(es), source: "
                  << c["source"].get<std::string>() << '\n';
    }
    std::cout << '\n';

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

int run_describe(const cli::ParsedArgs& a) {
    json s = build_state();
    bool as_json = false;
    auto it = a.flags.find("json");
    if (it != a.flags.end() && it->second) as_json = true;

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
        "  aliases, and (when run inside a luban project) vcpkg deps + luban.toml\n"
        "  prefs + build state.\n"
        "\n"
        "  Default output is human-readable text. With --json, prints a single\n"
        "  JSON object suitable for piping to jq / IDE plugins / future visualizers.";
    c.flags = {"json"};
    c.examples = {
        "luban describe\tHuman-readable summary",
        "luban describe --json\tMachine-readable JSON dump",
        "luban describe --json | jq .installed_components[].name\tJq filter",
    };
    c.run = run_describe;
    cli::register_subcommand(std::move(c));
}

}  // namespace luban::commands
