// `luban setup` — install toolchain components per selection.json.
//
// Orchestration:
//   1. ensure_dirs
//   2. deploy seed manifests + selection.json on first run
//   3. (optional) refresh bucket mirrors — currently per-manifest fetch only
//   4. install each enabled component
//   5. summarize failures + exit code
//
// Note: pre-v0.2 also wrote activate.{cmd,ps1,sh} into <data>/env. Removed —
// `luban env --user` (rustup-style) is the canonical PATH integration; CI
// uses `LUBAN_PREFIX=...` + the same. Activate scripts had no documented
// users by v0.2.

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>

#include "../bucket_sync.hpp"
#include "../cli.hpp"
#include "../component.hpp"
#include "../log.hpp"
#include "../paths.hpp"
#include "../scoop_manifest.hpp"
#include "../selection.hpp"

namespace luban::commands {

namespace {

const char* error_kind_name(component::ErrorKind k) {
    switch (k) {
        case component::ErrorKind::ManifestNotFound:    return "manifest-not-found";
        case component::ErrorKind::UnsafeManifest:      return "unsafe-manifest";
        case component::ErrorKind::IncompleteManifest:  return "incomplete-manifest";
        case component::ErrorKind::DownloadFailed:      return "download-failed";
        case component::ErrorKind::HashMismatch:        return "hash-mismatch";
        case component::ErrorKind::ExtractFailed:       return "extract-failed";
        case component::ErrorKind::Filesystem:          return "filesystem";
    }
    return "?";
}

std::vector<std::string> parse_csv(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < s.size()) {
        size_t end = s.find(',', start);
        if (end == std::string::npos) end = s.size();
        std::string name = s.substr(start, end - start);
        while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front()))) name.erase(name.begin());
        while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
        if (!name.empty()) out.push_back(name);
        if (end == s.size()) break;
        start = end + 1;
    }
    return out;
}

// Recursively expand `depends:` from each component's manifest, and
// topologically sort so deps appear before dependents in the install order.
// Manifests not findable are silently skipped (the install step surfaces a
// clearer error).
void expand_depends(std::vector<selection::Entry>& chosen) {
    // Pass 1: discover transitive dep graph.
    std::map<std::string, std::vector<std::string>> deps_of;
    std::vector<std::string> queue;
    for (auto& e : chosen) queue.push_back(e.name);

    while (!queue.empty()) {
        std::string name = std::move(queue.back());
        queue.pop_back();
        if (deps_of.count(name)) continue;
        deps_of[name] = {};
        auto fetched = bucket_sync::fetch_manifest(name);
        if (!fetched) continue;
        scoop_manifest::ResolvedManifest parsed;
        try {
            parsed = scoop_manifest::parse(fetched->manifest, name);
        } catch (...) { continue; }
        for (auto& dep : parsed.depends) {
            deps_of[name].push_back(dep);
            if (!deps_of.count(dep)) queue.push_back(dep);
        }
    }

    // Pass 2: DFS post-order — deps emit before their dependents.
    std::vector<std::string> sorted;
    std::set<std::string> visited;
    auto visit = [&](auto& self, const std::string& n) -> void {
        if (!visited.insert(n).second) return;
        auto it = deps_of.find(n);
        if (it != deps_of.end()) for (auto& d : it->second) self(self, d);
        sorted.push_back(n);
    };
    std::vector<std::string> initial;
    for (auto& e : chosen) initial.push_back(e.name);
    for (auto& n : initial) visit(visit, n);

    // Pass 3: rebuild chosen[] in sorted order; preserve enabled/note for
    // already-listed entries; fabricate enabled=true entries for new deps.
    std::map<std::string, selection::Entry> by_name;
    for (auto& e : chosen) by_name[e.name] = e;
    chosen.clear();
    for (auto& n : sorted) {
        auto it = by_name.find(n);
        if (it != by_name.end()) {
            chosen.push_back(it->second);
        } else {
            log::infof("   pulling in dep '{}' (transitive)", n);
            selection::Entry e;
            e.name = n;
            e.enabled = true;
            chosen.push_back(e);
        }
    }
}

int run_setup(const cli::ParsedArgs& a) {
    log::step("Ensuring canonical directories exist");
    paths::ensure_dirs();
    log::ok("directories ready");

    log::step("Deploying seed manifests + selection");
    auto overlays = selection::deploy_overlays();
    log::okf("overlays on disk: {}", overlays.size());
    auto sel = selection::load();

    bool dry_run = false;
    auto it_dry = a.flags.find("dry-run");
    if (it_dry != a.flags.end() && it_dry->second) dry_run = true;

    // --with / --without persist into <config>/selection.json BEFORE we build
    // the chosen set, so future `luban setup` (no flags) keeps the same set.
    std::vector<std::string> with_names;
    {
        auto it = a.opts.find("with");
        if (it != a.opts.end() && !it->second.empty()) with_names = parse_csv(it->second);
    }
    std::vector<std::string> without_names;
    {
        auto it = a.opts.find("without");
        if (it != a.opts.end() && !it->second.empty()) without_names = parse_csv(it->second);
    }
    bool selection_changed = false;
    for (auto& n : with_names) {
        if (selection::enable(sel, n)) {
            log::okf("{}enabled '{}' in selection.json",
                     dry_run ? "would have " : "", n);
            selection_changed = true;
        }
    }
    for (auto& n : without_names) {
        if (selection::disable(sel, n)) {
            log::okf("{}disabled '{}' in selection.json",
                     dry_run ? "would have " : "", n);
            selection_changed = true;
        }
        // Symmetric uninstall: --without removes from disk too. Idempotent.
        if (!dry_run && component::uninstall(n)) {
            log::okf("uninstalled '{}' (toolchain dir + shims removed)", n);
        }
    }
    if (!dry_run && selection_changed) selection::save(sel);

    // --only filter（可重复使用 -- ）
    std::vector<std::string> only;
    auto it_only = a.opts.find("only");
    if (it_only != a.opts.end() && !it_only->second.empty()) {
        only = parse_csv(it_only->second);
    }

    bool force = false;
    auto it_force = a.flags.find("force");
    if (it_force != a.flags.end() && it_force->second) force = true;

    auto in_only = [&](const std::string& n) {
        if (only.empty()) return true;
        return std::find(only.begin(), only.end(), n) != only.end();
    };

    std::vector<selection::Entry> chosen;
    for (auto& list : {std::cref(sel.components), std::cref(sel.extras)}) {
        for (auto& e : list.get()) {
            // `--only` 显式选项覆盖 enabled——用户说要装就装。
            // 没传 --only 时仍尊重 enabled。
            if (only.empty()) {
                if (e.enabled) chosen.push_back(e);
            } else {
                if (in_only(e.name)) chosen.push_back(e);
            }
        }
    }
    // 若 --only 指定了 selection 里没有的名字，也尝试装（让 overlay-only 组件能 setup）。
    if (!only.empty()) {
        for (auto& n : only) {
            bool found = false;
            for (auto& e : chosen) if (e.name == n) { found = true; break; }
            if (!found) {
                selection::Entry synthetic;
                synthetic.name = n;
                synthetic.enabled = true;
                chosen.push_back(synthetic);
            }
        }
    }
    if (chosen.empty()) {
        log::warn("nothing to install \xe2\x80\x94 selection.json has no enabled entries.");
        return 0;
    }

    // Dependency closure: pull in any `depends:` entries declared in each
    // chosen component's manifest. Common case: emscripten depends on node.
    expand_depends(chosen);

    log::stepf("Installing {} component(s)", chosen.size());
    std::vector<std::pair<std::string, std::string>> failures;
    for (auto& entry : chosen) {
        log::stepf("\xe2\x86\x92 {}", entry.name);
        if (dry_run) {
            log::infof("   would install {} (dry-run; skipping fetch+download+install)", entry.name);
            continue;
        }
        auto rc = component::install(entry.name, force);
        if (!rc) {
            log::errf("   install of {} failed: {}: {}",
                      entry.name, error_kind_name(rc.error().kind), rc.error().message);
            failures.emplace_back(entry.name, rc.error().message);
        }
    }

    if (!failures.empty()) {
        log::warnf("{} component(s) failed:", failures.size());
        for (auto& [n, msg] : failures) log::warnf("  - {}: {}", n, msg);
        return 1;
    }

    log::ok("setup complete");
    log::infof("shims written to {}", paths::bin_dir().string());
    log::info("run `luban env --user` to register on HKCU PATH (rustup-style).");
    return 0;
}

}  // namespace

void register_setup() {
    cli::Subcommand c;
    c.name = "setup";
    c.help = "install toolchain (LLVM-MinGW + cmake + ninja + mingit + vcpkg)";
    c.group = "setup";
    c.long_help =
        "  Install all enabled toolchain components per <config>/luban/selection.json.\n"
        "  Idempotent — re-running is fast unless --force.\n"
        "\n"
        "  --with X[,Y]      enable + install component(s) and persist into\n"
        "                    selection.json. Recursively pulls in `depends:`.\n"
        "  --without X[,Y]   disable in selection + uninstall (toolchain dir\n"
        "                    and shims). Symmetric to --with.\n"
        "  --only  X[,Y]     this run only — install just X; does not persist.\n"
        "\n"
        "  Shims land in bin_dir() (~/.local/bin by default). After install,\n"
        "  run `luban env --user` once to register that dir on HKCU PATH —\n"
        "  any new shell will see cmake/clang/ninja/etc. directly. CI users\n"
        "  pin LUBAN_PREFIX=... and source the same.";
    c.opts = {{"only", ""}, {"with", ""}, {"without", ""}};
    c.flags = {"dry-run", "refresh-buckets", "force"};
    c.examples = {
        "luban setup\tInstall all enabled components",
        "luban setup --only vcpkg\tInstall just vcpkg (overrides selection.enabled)",
        "luban setup --only ninja --force\tForce reinstall of ninja",
        "luban setup --dry-run\tShow what would be installed",
        "luban setup --with emscripten\tInstall emscripten + deps; persist in selection.json",
        "luban setup --without zig\tDisable + uninstall zig",
    };
    c.run = run_setup;
    cli::register_subcommand(std::move(c));
}

}  // namespace luban::commands
