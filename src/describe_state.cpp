// See `describe_state.hpp` for design rationale + schema.

#include "describe_state.hpp"

#include <filesystem>
#include <system_error>

#include "luban/version.hpp"
#include "luban_toml.hpp"
#include "paths.hpp"
#include "vcpkg_manifest.hpp"

namespace luban::describe_state {

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

// Walk cwd upward to the first dir holding `luban.cmake` or `vcpkg.json`.
// Empty path = not in a luban project.
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

}  // namespace

json build() {
    json out;

    // ---- system tier ----
    out["schema"] = 1;
    out["luban_version"] = std::string(luban::kLubanVersion);

    json paths_obj;
    for (auto& [name, p] : paths::all_dirs()) paths_obj[name] = p.string();
    out["paths"] = paths_obj;

    // installed_components was a v0.x notion — `registry::load_installed`
    // walked installed.json. Removed in v0.5.0 (DESIGN §11 drops "tool
    // install" / generation history). The bp source registry under
    // <config>/luban/sources.toml + <state>/luban/applied.txt is the
    // authoritative successor; expose it via `bp source` / `bp list`.
    out["installed_components"] = json::array();

    // ---- project tier (if cwd is in a luban project) ----
    fs::path proj = find_project_root();
    if (!proj.empty()) {
        json p;
        p["root"] = proj.string();

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

}  // namespace luban::describe_state
