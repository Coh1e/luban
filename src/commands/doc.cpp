// `luban doc` — generate API docs for a luban-managed project (cargo doc-style).
//
// Wraps doxygen with sensible defaults so users don't author a Doxyfile by
// hand. Two passes:
//
//   1. If no Doxyfile exists at the project root, materialize one from
//      `templates/Doxyfile.tmpl`, filling project_name / version / brief
//      from luban.toml + vcpkg.json (best-effort).
//
//   2. Spawn doxygen via env_snapshot::apply_to so it picks up any
//      luban-installed doxygen on PATH. Output lands at
//      `<project>/build/doc/html/index.html`.
//
// `--open` opens the generated index in the user's default browser.
// `--clean` deletes `<project>/build/doc/` and exits.
//
// Doxygen is NOT in `main/cpp-base` — install it via your OS package
// manager (`scoop install doxygen` / `apt install doxygen` / `brew install
// doxygen`) or write a user blueprint. A doxygen entry in cpp-base is on
// the v1.x list (docs/FUTURE.md).

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

#include "json.hpp"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#include "../cli.hpp"
#include "../env_snapshot.hpp"
#include "../file_util.hpp"
#include "../log.hpp"
#include "../luban_toml.hpp"
#include "../paths.hpp"
#include "../proc.hpp"
#include "../registry.hpp"

namespace luban::commands {

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

// Walk up from cwd to find a directory containing vcpkg.json or
// CMakeLists.txt — that's "the project". Mirrors what luban add / build do.
fs::path find_project_root() {
    fs::path cur = fs::current_path();
    std::error_code ec;
    while (!cur.empty()) {
        if (fs::exists(cur / "vcpkg.json", ec) || fs::exists(cur / "CMakeLists.txt", ec)) {
            return cur;
        }
        fs::path parent = cur.parent_path();
        if (parent == cur) break;
        cur = parent;
    }
    return fs::current_path();
}

// Locate templates/Doxyfile.tmpl. Mirrors selection.cpp::seed_root /
// manifest_source.cpp::seed_root layout — luban.exe ships with a sibling
// templates/ directory.
fs::path find_doxyfile_template() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4];
    DWORD got = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    fs::path exe = (got == 0) ? fs::current_path() / "luban.exe"
                              : fs::path(std::wstring(buf, got));
#else
    fs::path exe = fs::current_path() / "luban";
#endif
    fs::path d = exe.parent_path();
    std::vector<fs::path> candidates = {
        d / "templates" / "Doxyfile.tmpl",
        d.parent_path() / "templates" / "Doxyfile.tmpl",
        d.parent_path().parent_path() / "templates" / "Doxyfile.tmpl",
    };
    std::error_code ec;
    for (auto& c : candidates) {
        if (fs::exists(c, ec)) return c;
    }
    return {};
}

// Best-effort project metadata sniffer. Looks at luban.toml first
// (canonical for luban-scaffolded projects), falls back to vcpkg.json's
// name/version, then to the directory name.
struct ProjectMeta {
    std::string name;
    std::string version;
    std::string brief;
};

ProjectMeta sniff_project_meta(const fs::path& root) {
    ProjectMeta m;
    m.name = root.filename().string();
    m.version = "0.1.0";
    m.brief = "C++ project";

    // vcpkg.json gives us name + version-string for free.
    std::error_code ec;
    fs::path vcpkg_json = root / "vcpkg.json";
    if (fs::exists(vcpkg_json, ec)) {
        try {
            std::string text = file_util::read_text_no_bom(vcpkg_json);
            json doc = json::parse(text);
            if (doc.contains("name") && doc["name"].is_string()) {
                m.name = doc["name"].get<std::string>();
            }
            // Both `version` and `version-string` show up in the wild.
            for (auto* k : {"version-string", "version", "version-semver"}) {
                if (doc.contains(k) && doc[k].is_string()) {
                    m.version = doc[k].get<std::string>();
                    break;
                }
            }
            if (doc.contains("description") && doc["description"].is_string()) {
                m.brief = doc["description"].get<std::string>();
            }
        } catch (...) {
            // Malformed vcpkg.json — silently fall back to defaults.
        }
    }
    return m;
}

// Naive {{key}} substitution. Same machinery used by new_project's template
// engine; intentionally not regex / mustache to keep dependencies tight.
std::string render_template(std::string text,
                            const std::map<std::string, std::string>& ctx) {
    for (auto& [k, v] : ctx) {
        std::string needle = "{{" + k + "}}";
        size_t pos = 0;
        while ((pos = text.find(needle, pos)) != std::string::npos) {
            text.replace(pos, needle.size(), v);
            pos += v.size();
        }
    }
    return text;
}

// Materialize Doxyfile from template + project metadata. Idempotent — never
// overwrites an existing Doxyfile (so user customizations survive `luban doc`
// re-runs); returns true if it newly created the file.
bool ensure_doxyfile(const fs::path& project_root) {
    fs::path target = project_root / "Doxyfile";
    std::error_code ec;
    if (fs::exists(target, ec)) return false;

    fs::path tpl = find_doxyfile_template();
    if (tpl.empty()) {
        log::err("could not locate Doxyfile.tmpl alongside luban.exe (templates/ dir missing?)");
        return false;
    }

    auto meta = sniff_project_meta(project_root);
    std::map<std::string, std::string> ctx = {
        {"project_name", meta.name},
        {"project_version", meta.version},
        {"project_brief", meta.brief},
    };

    std::string tpl_text = file_util::read_text(tpl);
    std::string out = render_template(tpl_text, ctx);

    file_util::write_text_atomic(target, out);
    log::okf("wrote {} (edit freely; `luban doc` won't overwrite it)", target.string());
    return true;
}

// Open a path in the user's default browser. Best-effort; any failure is
// non-fatal (the docs were still generated and the path was printed).
void open_in_browser(const fs::path& p) {
#ifdef _WIN32
    ShellExecuteW(nullptr, L"open", p.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    std::string cmd = "xdg-open " + p.string() + " 2>/dev/null || open " + p.string() + " 2>/dev/null";
    std::system(cmd.c_str());
#endif
}

int run_doc(const cli::ParsedArgs& a) {
    bool open = a.flags.count("open") && a.flags.at("open");
    bool clean = a.flags.count("clean") && a.flags.at("clean");

    fs::path root = find_project_root();
    fs::path build_doc = root / "build" / "doc";
    std::error_code ec;

    if (clean) {
        if (fs::exists(build_doc, ec)) {
            fs::remove_all(build_doc, ec);
            log::okf("removed {}", build_doc.string());
        } else {
            log::infof("nothing to clean ({} doesn't exist)", build_doc.string());
        }
        return 0;
    }

    ensure_doxyfile(root);

    // Spawn doxygen. env_snapshot::apply_to merges luban's PATH overlay so
    // a luban-installed doxygen (when one lands in a future blueprint)
    // would take precedence.
    auto env_overrides = env_snapshot::apply_to({});
    std::vector<std::string> cmd = {"doxygen", "Doxyfile"};
    log::stepf("running doxygen in {}", root.string());
    int rc = proc::run(cmd, root.string(), env_overrides);
    if (rc != 0) {
        log::errf("doxygen exited with code {}", rc);
        log::info("not on PATH? install via your OS package manager (scoop/apt/brew) and retry.");
        return rc;
    }

    fs::path index = build_doc / "html" / "index.html";
    if (fs::exists(index, ec)) {
        log::okf("docs generated at {}", index.string());
        if (open) {
            open_in_browser(index);
        } else {
            log::info("re-run with --open to view in browser.");
        }
    } else {
        log::warnf("doxygen finished but {} not found — check Doxyfile", index.string());
    }
    return 0;
}

}  // namespace

void register_doc() {
    cli::Subcommand c;
    c.name = "doc";
    c.help = "generate project API docs via doxygen (cargo doc-style)";
    c.group = "project";
    c.long_help =
        "  `luban doc` runs doxygen on the current project and writes HTML\n"
        "  output to build/doc/html/. On first run, materializes a default\n"
        "  Doxyfile from luban's template (sniffs project name/version from\n"
        "  vcpkg.json); subsequent runs reuse the file — so user edits stick.\n"
        "\n"
        "  Doxygen is not bundled with luban. Install via your OS package\n"
        "  manager (scoop install doxygen / apt install doxygen / brew install\n"
        "  doxygen) or wait for the future cpp-base doxygen entry.\n"
        "\n"
        "  --open    Open build/doc/html/index.html in the default browser.\n"
        "  --clean   Delete build/doc/ and exit (no doxygen run).";
    c.flags = {"open", "clean"};
    c.examples = {
        "luban doc\tGenerate (or regenerate) HTML docs",
        "luban doc --open\tGenerate and open index.html",
        "luban doc --clean\tWipe build/doc/",
    };
    c.run = run_doc;
    cli::register_subcommand(std::move(c));
}

}  // namespace luban::commands
