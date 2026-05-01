// `luban specs` — scaffold AGENTS.md + specs/ for AI-driven requirement
// collection (SAGE method).
//
// luban does NOT call any AI itself. This verb materializes the artifacts
// that an AI agent (Claude Code, Cursor, etc.) reads at the project root:
//
//   - AGENTS.md           — project-level guidance with luban-managed
//                           sections (rendered from luban.toml + vcpkg.json
//                           + installed.json) and user-owned sections.
//   - CLAUDE.md           — one-line indirection to AGENTS.md for tools
//                           that hard-code the CLAUDE.md filename.
//   - specs/README.md     — explanation that the layout is a suggestion.
//   - specs/HOW-TO-USE.md — same content as `luban specs --help` long
//                           form (single source via cmake embed_text).
//   - specs/sage/         — empty dir; populated by `specs new <topic>`.
//
// Subcommands:
//   init                  — first-time scaffold; idempotent (won't clobber
//                           existing AGENTS.md / CLAUDE.md / specs/).
//   sync                  — re-render the luban-managed sections of an
//                           existing AGENTS.md; user-edited sections
//                           outside marker blocks are preserved verbatim.
//   new <topic>           — create specs/sage/<topic>/{scene,pain,mvp}.md.
//   status                — list SAGE units with completeness heuristic.
//
// Marker block protocol:
//   <!-- BEGIN luban-managed: <name> -->
//   ...content...
//   <!-- END luban-managed -->
// luban only rewrites text between matching markers. Removing the markers
// opts that section out of future syncs entirely.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include "../util/win.hpp"
#endif

#include "json.hpp"
#include "luban/embedded_help/specs.hpp"  // luban::embedded_help::specs_help

#include "../cli.hpp"
#include "../log.hpp"
#include "../luban_toml.hpp"
#include "../paths.hpp"
#include "../registry.hpp"

namespace luban::commands {

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

// ---- project root + template root ------------------------------------------

// Walk up from cwd looking for the project sentinel. Mirrors `luban add`'s
// behavior: vcpkg.json wins, CMakeLists.txt is the fallback. If neither is
// found (running outside a project), specs init still works — it just uses
// the current dir.
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

// Locate templates/specs/ alongside luban.exe. Three candidate paths to
// match the install (luban.exe + templates/) and dev (build/release/luban.exe
// + ../templates/) layouts.
fs::path find_specs_template_root() {
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
        d / "templates" / "specs",
        d.parent_path() / "templates" / "specs",
        d.parent_path().parent_path() / "templates" / "specs",
    };
    std::error_code ec;
    for (auto& c : candidates) {
        if (fs::is_directory(c, ec)) return c;
    }
    return {};
}

// ---- file IO helpers --------------------------------------------------------

std::string read_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool write_text_atomic(const fs::path& p, const std::string& content) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p; tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    fs::rename(tmp, p, ec);
    if (ec) {
        // Cross-volume rename fallback.
        fs::copy_file(tmp, p, fs::copy_options::overwrite_existing, ec);
        fs::remove(tmp, ec);
    }
    return !ec;
}

// ---- {{placeholder}} substitution ------------------------------------------

// Naive replace-all on `{{key}}` tokens. Same machinery as new_project's
// template engine — kept local to this TU rather than abstracted so each
// caller can pick its own context shape.
std::string render(std::string text,
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

// ---- project-context renderer ----------------------------------------------

// Sniff the project's luban.toml + vcpkg.json + installed.json into the
// context map for AGENTS.md rendering. Best-effort: missing files become
// "(none)" placeholders rather than blocking the render.
std::map<std::string, std::string> build_project_context(const fs::path& root) {
    std::map<std::string, std::string> ctx;

    // luban.toml — cpp / triplet / sanitizers + warnings.
    auto cfg = luban_toml::load(root / "luban.toml");
    ctx["cpp_std"] = std::to_string(cfg.project.cpp);
    ctx["triplet"] = cfg.project.triplet.empty() ? "x64-mingw-static" : cfg.project.triplet;
    if (cfg.scaffold.sanitizers.empty()) {
        ctx["sanitizers"] = "(none)";
    } else {
        std::string s;
        for (size_t i = 0; i < cfg.scaffold.sanitizers.size(); ++i) {
            if (i) s += ", ";
            s += cfg.scaffold.sanitizers[i];
        }
        ctx["sanitizers"] = s;
    }

    // installed.json — toolchain summary (just names + versions, comma list).
    auto recs = registry::load_installed();
    if (recs.empty()) {
        ctx["toolchain_summary"] = "(no luban toolchain installed)";
    } else {
        std::string s;
        bool first = true;
        for (auto& [name, rec] : recs) {
            if (!first) s += ", ";
            first = false;
            s += name + " " + rec.version;
        }
        ctx["toolchain_summary"] = s;
    }

    // vcpkg.json — deps list (name + optional version-range).
    std::error_code ec;
    fs::path vcjson = root / "vcpkg.json";
    if (fs::exists(vcjson, ec)) {
        try {
            std::ifstream in(vcjson);
            json doc;
            in >> doc;
            std::string s;
            if (doc.contains("dependencies") && doc["dependencies"].is_array()) {
                for (auto& d : doc["dependencies"]) {
                    if (!s.empty()) s += ", ";
                    if (d.is_string()) s += d.get<std::string>();
                    else if (d.is_object() && d.contains("name")) {
                        s += d["name"].get<std::string>();
                    }
                }
            }
            ctx["deps_list"] = s.empty() ? std::string("(none)") : s;
        } catch (...) {
            ctx["deps_list"] = "(vcpkg.json parse error)";
        }
    } else {
        ctx["deps_list"] = "(no vcpkg.json)";
    }

    // CMakeLists targets — rough sniff via grep, since reading cmake fully
    // is out of scope. Looks for `add_executable(<name>` / `add_library(<name>`.
    fs::path cmakelists = root / "CMakeLists.txt";
    if (fs::exists(cmakelists, ec)) {
        std::string body = read_text(cmakelists);
        std::regex re(R"((add_executable|add_library)\s*\(\s*([A-Za-z_][A-Za-z0-9_-]*))");
        std::vector<std::string> targets;
        for (auto it = std::sregex_iterator(body.begin(), body.end(), re);
             it != std::sregex_iterator(); ++it) {
            targets.push_back((*it)[2].str());
        }
        if (targets.empty()) {
            ctx["targets"] = "(none detected)";
        } else {
            std::string s;
            for (size_t i = 0; i < targets.size(); ++i) {
                if (i) s += ", ";
                s += targets[i];
            }
            ctx["targets"] = s;
        }
    } else {
        ctx["targets"] = "(no CMakeLists.txt)";
    }

    // UB section — depend on whether UBSan is configured.
    bool has_ubsan = false;
    for (auto& s : cfg.scaffold.sanitizers) {
        if (s == "ub" || s == "undefined") { has_ubsan = true; break; }
    }
    std::string ub;
    if (has_ubsan) {
        ub = "- UBSan is enabled in luban.toml — running tests under it is cheap;\n"
             "  agents should suggest `luban build` followed by execution to surface\n"
             "  signed overflow / out-of-bounds / strict-aliasing bugs.";
    } else {
        ub = "- UBSan is NOT configured in this project. To turn on, add\n"
             "  `sanitizers = [\"ub\"]` to luban.toml `[scaffold]` and rebuild.\n"
             "- Common gotchas regardless: signed overflow, out-of-bounds reads,\n"
             "  use-after-free / use-after-move, strict aliasing.";
    }
    ctx["ub_section"] = ub;

    return ctx;
}

// ---- marker block engine ---------------------------------------------------

// Section names luban manages. If a marker block with one of these names
// exists in the AGENTS.md, sync rewrites its content. If the marker block
// is absent, sync skips that section (user opted out by removing markers).
const std::vector<std::string>& managed_section_order() {
    static const std::vector<std::string> v = {
        "project-context",
        "cpp-modernization",
        "ub-perf-guidance",
    };
    return v;
}

// Re-render the managed sections of an existing AGENTS.md. The new content
// for each section comes from the template (rendered with current project
// context). Returns updated text; empty string on failure.
std::string sync_managed_blocks(const std::string& existing,
                                const std::map<std::string, std::string>& tpl_blocks) {
    std::string out = existing;

    for (auto& section : managed_section_order()) {
        // Build markers we're matching. We allow surrounding whitespace
        // tolerance but require exact section name match.
        std::string begin_marker = "<!-- BEGIN luban-managed: " + section + " -->";
        std::string end_marker   = "<!-- END luban-managed -->";

        // Find the next BEGIN for this specific section. We don't iterate
        // multiple instances — duplicates would be a user authoring bug
        // and silently rewriting all of them is more confusing than the
        // single-instance contract.
        size_t b = out.find(begin_marker);
        if (b == std::string::npos) continue;  // user removed → skip

        // END marker must come AFTER the BEGIN. We pick the nearest END
        // (so nested marker blocks don't break the algorithm).
        size_t e = out.find(end_marker, b + begin_marker.size());
        if (e == std::string::npos) {
            log::warnf("AGENTS.md: '{}' has no matching END marker; skipping sync", section);
            continue;
        }

        // Replace from end of BEGIN line through end of END marker exclusive
        // — i.e., the body, preserving the markers themselves.
        size_t body_start = b + begin_marker.size();
        // Skip immediate trailing newline after BEGIN if present (so
        // we keep one-line separation between marker and content).
        if (body_start < out.size() && out[body_start] == '\n') {
            body_start += 1;
        } else if (body_start + 1 < out.size()
                   && out[body_start] == '\r' && out[body_start + 1] == '\n') {
            body_start += 2;
        }

        auto it = tpl_blocks.find(section);
        if (it == tpl_blocks.end()) continue;  // template didn't have this section

        // Replace [body_start, e) with the new content + a trailing \n
        // before the END marker (so the END marker stays on its own line).
        std::string new_body = it->second;
        if (new_body.empty() || new_body.back() != '\n') new_body.push_back('\n');
        out.replace(body_start, e - body_start, new_body);
    }

    return out;
}

// Extract the per-section bodies from a freshly-rendered template (used as
// the source of truth for sync). Returns map { section-name → body-text }.
std::map<std::string, std::string> extract_template_blocks(const std::string& rendered) {
    std::map<std::string, std::string> out;
    std::regex re(R"(<!-- BEGIN luban-managed: ([a-z0-9-]+) -->\s*\n([\s\S]*?)<!-- END luban-managed -->)",
                  std::regex::ECMAScript);
    auto begin = std::sregex_iterator(rendered.begin(), rendered.end(), re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        std::string section = (*it)[1].str();
        std::string body    = (*it)[2].str();
        out[section] = std::move(body);
    }
    return out;
}

// ---- subcommands -----------------------------------------------------------

int run_init(const fs::path& project_root, const fs::path& tpl_root) {
    fs::path agents = project_root / "AGENTS.md";
    fs::path claude = project_root / "CLAUDE.md";
    fs::path specs_dir = project_root / "specs";
    fs::path how_to = specs_dir / "HOW-TO-USE.md";
    fs::path readme = specs_dir / "README.md";
    fs::path sage_dir = specs_dir / "sage";

    std::error_code ec;
    auto ctx = build_project_context(project_root);

    // AGENTS.md — refuse to clobber if exists. Tell user to use `sync`.
    if (fs::exists(agents, ec)) {
        log::warnf("AGENTS.md already exists at {}; use `luban specs sync` to refresh managed sections",
                   agents.string());
    } else {
        std::string tpl = read_text(tpl_root / "AGENTS.md.tmpl");
        if (tpl.empty()) {
            log::errf("could not read AGENTS.md.tmpl from {}", tpl_root.string());
            return 1;
        }
        write_text_atomic(agents, render(std::move(tpl), ctx));
        log::okf("wrote {}", agents.string());
    }

    // CLAUDE.md — tiny indirection. Refuse to clobber.
    if (fs::exists(claude, ec)) {
        log::infof("CLAUDE.md already exists at {}; left alone", claude.string());
    } else {
        std::string tpl = read_text(tpl_root / "CLAUDE.md.tmpl");
        if (!tpl.empty()) {
            write_text_atomic(claude, tpl);
            log::okf("wrote {}", claude.string());
        }
    }

    // specs/ scaffold.
    fs::create_directories(sage_dir, ec);
    if (!fs::exists(readme, ec)) {
        std::string tpl = read_text(tpl_root / "README.md.tmpl");
        if (!tpl.empty()) {
            write_text_atomic(readme, tpl);
            log::okf("wrote {}", readme.string());
        }
    }
    if (!fs::exists(how_to, ec)) {
        // HOW-TO-USE.md content is shared with `luban specs --help` long_help
        // via the cmake-embedded specs.md. Single source of truth = same
        // file in both views.
        write_text_atomic(how_to, embedded_help::specs_help);
        log::okf("wrote {}", how_to.string());
    }
    // .gitkeep so the empty sage/ dir survives git checkout.
    fs::path gitkeep = sage_dir / ".gitkeep";
    if (!fs::exists(gitkeep, ec)) {
        write_text_atomic(gitkeep, "");
    }

    log::ok("specs scaffolded — open AGENTS.md to verify, then start chatting with your AI agent");
    return 0;
}

int run_sync(const fs::path& project_root, const fs::path& tpl_root) {
    fs::path agents = project_root / "AGENTS.md";
    std::error_code ec;
    if (!fs::exists(agents, ec)) {
        log::errf("AGENTS.md not found at {}; run `luban specs init` first", agents.string());
        return 1;
    }

    auto ctx = build_project_context(project_root);
    std::string tpl = read_text(tpl_root / "AGENTS.md.tmpl");
    if (tpl.empty()) {
        log::errf("could not read AGENTS.md.tmpl from {}", tpl_root.string());
        return 1;
    }
    std::string fresh_rendered = render(std::move(tpl), ctx);
    auto tpl_blocks = extract_template_blocks(fresh_rendered);

    std::string existing = read_text(agents);
    std::string updated = sync_managed_blocks(existing, tpl_blocks);

    if (updated == existing) {
        log::info("AGENTS.md already up to date — no managed sections needed rewriting");
        return 0;
    }

    write_text_atomic(agents, updated);
    log::okf("refreshed luban-managed sections in {}", agents.string());
    return 0;
}

int run_new(const fs::path& project_root, const fs::path& tpl_root,
            const std::string& topic) {
    if (topic.empty()) {
        log::err("usage: luban specs new <topic>");
        return 2;
    }
    // Sanitize: kebab-case lowercase letters / digits / dash / underscore only.
    static const std::regex valid_re("^[a-z0-9][a-z0-9_-]*$");
    if (!std::regex_match(topic, valid_re)) {
        log::errf("'{}' is not a valid topic name (lowercase, digits, '-', '_'; must start alnum)", topic);
        return 2;
    }

    fs::path target = project_root / "specs" / "sage" / topic;
    std::error_code ec;
    if (fs::exists(target, ec)) {
        log::errf("specs/sage/{} already exists; pick a different topic or edit the existing one", topic);
        return 1;
    }
    fs::create_directories(target, ec);

    std::map<std::string, std::string> ctx = {{"topic", topic}};
    for (auto* name : {"scene.md", "pain.md", "mvp.md"}) {
        std::string tpl = read_text(tpl_root / "sage-stub" / (std::string(name) + ".tmpl"));
        if (tpl.empty()) {
            log::warnf("template {} missing — wrote empty file", name);
            tpl = "";
        }
        fs::path out = target / name;
        write_text_atomic(out, render(tpl, ctx));
        log::okf("wrote {}", out.string());
    }
    log::infof("now: open {} and walk the user through scene → pain → mvp.", target.string());
    return 0;
}

int run_status(const fs::path& project_root) {
    fs::path sage_dir = project_root / "specs" / "sage";
    std::error_code ec;
    if (!fs::is_directory(sage_dir, ec)) {
        log::infof("no specs/sage/ at {}; run `luban specs init` first", project_root.string());
        return 0;
    }

    int total = 0;
    for (auto& entry : fs::directory_iterator(sage_dir, ec)) {
        if (!entry.is_directory()) continue;
        const auto& topic = entry.path().filename().string();
        if (topic.starts_with(".")) continue;  // skip .gitkeep, dotdirs

        ++total;
        // Completeness heuristic: count "real content" lines, defined as
        // non-empty lines that aren't comment markers and don't start with
        // a parenthesis (the stub templates use `(...)` paragraphs as
        // placeholder body). A SAGE step is "filled" once it has 3+ such
        // lines — enough to be a real paragraph, not just a stub edit.
        auto filled = [&](const char* name) -> std::string {
            fs::path p = entry.path() / name;
            if (!fs::exists(p, ec)) return "missing";
            std::ifstream in(p);
            std::string line;
            int real_lines = 0;
            bool in_html_comment = false;
            while (std::getline(in, line)) {
                // Strip trailing CR.
                while (!line.empty() && line.back() == '\r') line.pop_back();
                // Track multi-line HTML comments (`<!-- ... -->`).
                if (line.find("<!--") != std::string::npos) in_html_comment = true;
                bool comment_line = in_html_comment;
                if (line.find("-->") != std::string::npos) in_html_comment = false;
                if (comment_line) continue;
                // Trim leading whitespace.
                auto first = line.find_first_not_of(" \t");
                if (first == std::string::npos) continue;
                line = line.substr(first);
                if (line.empty()) continue;
                if (line[0] == '#') continue;          // markdown headings
                if (line[0] == '(') continue;          // stub placeholder text
                if (line[0] == '-' && line.size() < 4) continue;  // empty bullets `-`
                ++real_lines;
                if (real_lines >= 3) return "✓";
            }
            return real_lines == 0 ? "stub" : "partial";
        };
        log::infof("  {}: scene={}, pain={}, mvp={}", topic,
                   filled("scene.md"), filled("pain.md"), filled("mvp.md"));
    }
    if (total == 0) {
        log::info("no SAGE units yet; create one with `luban specs new <topic>`");
    } else {
        log::okf("{} SAGE unit(s) total", total);
        if (total >= 2) {
            log::info("hint: 2+ units — ask your AI agent to compose them into specs/composed.md");
        }
    }
    return 0;
}

int run_specs(const cli::ParsedArgs& a) {
    if (a.positional.empty()) {
        log::err("usage: luban specs <init | sync | new <topic> | status>");
        log::info("for the full manual: luban specs --help");
        return 2;
    }

    fs::path project_root = find_project_root();
    fs::path tpl_root = find_specs_template_root();
    if (tpl_root.empty()) {
        log::err("could not locate templates/specs/ alongside luban.exe");
        log::info("(rebuild luban with the templates/ directory installed beside it)");
        return 1;
    }

    const std::string& sub = a.positional[0];
    if (sub == "init")   return run_init(project_root, tpl_root);
    if (sub == "sync")   return run_sync(project_root, tpl_root);
    if (sub == "status") return run_status(project_root);
    if (sub == "new") {
        std::string topic = a.positional.size() >= 2 ? a.positional[1] : std::string();
        return run_new(project_root, tpl_root, topic);
    }

    log::errf("unknown specs subcommand '{}' (expected init|sync|new|status)", sub);
    return 2;
}

}  // namespace

void register_specs() {
    cli::Subcommand c;
    c.name = "specs";
    c.help = "scaffold AGENTS.md + specs/ for AI-driven requirement collection";
    c.group = "project";
    c.long_help = embedded_help::specs_help;
    // <subcommand> is required; <topic> is only consumed by `new`.
    // cli enforces minimum positional count; we hand-validate the topic
    // inside run_new since most subcommands don't need it.
    c.n_positional = 1;
    c.positional_names = {"sub"};
    c.examples = {
        "luban specs init\tFirst-time scaffold (AGENTS.md + specs/)",
        "luban specs sync\tRefresh AGENTS.md's luban-managed sections",
        "luban specs new onboarding\tStart a SAGE unit at specs/sage/onboarding/",
        "luban specs status\tList SAGE units and their fill state",
    };
    c.run = run_specs;
    cli::register_subcommand(std::move(c));
}

}  // namespace luban::commands
