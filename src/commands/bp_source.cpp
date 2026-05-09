// `commands/bp_source` — implements the `bp source` and `bp search`
// sub-verb families on top of source_registry (DESIGN.md §9.10).
//
// Routing: top-level dispatch happens in commands/blueprint.cpp; this TU
// exposes two entry points (run_bp_source / run_bp_search) that consume
// the already-sliced positional list. We don't register any top-level
// CLI verbs of our own — `bp source` is logically a deeper layer of the
// existing `bp` group.
//
// What each verb does (matches §9.10.3):
//   add    register + first-fetch a source (github tarball or file:// passthrough)
//   rm     drop registration + remove the local copy (github sources only)
//   ls     list registered sources with current commit
//   update re-fetch one or all sources, refresh commit field
//   search scan registered sources + user-local for blueprints matching a pattern
//
// Implementation policy:
//   - Tarball-only fetch for github; no real `git clone` (matches the
//     §9.10.5 "first-time tarball" path; full git mode is v1.x).
//   - file:// URLs are *live links* — we don't copy the directory, just
//     record the URL. `bp apply` reads through the URL straight to the
//     user's repo, so editing the .lua there is immediately visible
//     without `bp source update`.
//   - Trust prompt on `add` (interactive). Use `--yes` to skip.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "json.hpp"

#include "../archive.hpp"
#include "../cli.hpp"
#include "../download.hpp"
#include "../iso_time.hpp"
#include "../paths.hpp"
#include "../source_registry.hpp"
#include "bp_source.hpp"

namespace luban::commands {

namespace {

namespace fs = std::filesystem;
namespace sr = luban::source_registry;

// ---- URL parsing -------------------------------------------------------

struct GithubRef {
    std::string owner;
    std::string repo;
};

std::optional<GithubRef> parse_github_url(std::string_view url) {
    constexpr std::string_view kPrefix = "https://github.com/";
    if (!url.starts_with(kPrefix)) return std::nullopt;
    auto rest = url.substr(kPrefix.size());
    // Strip trailing slash + optional .git suffix.
    if (!rest.empty() && rest.back() == '/') rest.remove_suffix(1);
    if (rest.ends_with(".git")) rest.remove_suffix(4);
    auto slash = rest.find('/');
    if (slash == std::string_view::npos) return std::nullopt;
    auto trailing = rest.find('/', slash + 1);
    if (trailing != std::string_view::npos) {
        // URLs like https://github.com/o/r/tree/main are not what we want;
        // we ask for repo-root URLs only. Surface the malformed input loudly.
        return std::nullopt;
    }
    GithubRef out;
    out.owner = std::string(rest.substr(0, slash));
    out.repo = std::string(rest.substr(slash + 1));
    if (out.owner.empty() || out.repo.empty()) return std::nullopt;
    return out;
}

/// Convert `file:///D:/path/to/repo` (or `file://D:/...`) to a local fs::path.
/// On Windows, both `file:///D:/foo` and `file://D:/foo` are accepted in the
/// wild; tolerate both.
std::optional<fs::path> parse_file_url(std::string_view url) {
    constexpr std::string_view kFileScheme = "file://";
    if (!url.starts_with(kFileScheme)) return std::nullopt;
    auto rest = url.substr(kFileScheme.size());
    // Drop one extra leading slash if present (`file:///D:/...`).
    if (rest.starts_with("/") && rest.size() >= 4 &&
        std::isalpha(static_cast<unsigned char>(rest[1])) && rest[2] == ':') {
        rest.remove_prefix(1);
    }
    if (rest.empty()) return std::nullopt;
    return fs::path(std::string(rest));
}

bool looks_like_local_path(std::string_view input) {
    // Windows drive: `<letter>:[\/]...`
    if (input.size() >= 3 &&
        std::isalpha(static_cast<unsigned char>(input[0])) &&
        input[1] == ':' && (input[2] == '\\' || input[2] == '/')) {
        return true;
    }
    // POSIX absolute (`/...`) or UNC (`\\server\...`).
    if (input.starts_with("/") || input.starts_with("\\\\")) return true;
    return false;
}

/// Extract the trailing path segment of a path-like string. Used to default
/// a source name from a github repo or filesystem dir.
std::string basename_of(std::string_view path_like) {
    auto pos = path_like.find_last_of("/\\");
    auto base = (pos == std::string_view::npos) ? path_like : path_like.substr(pos + 1);
    if (base.ends_with(".git")) base.remove_suffix(4);
    return std::string(base);
}

struct AddTarget {
    std::string url;        ///< Canonical URL stored in registry.
    std::string default_name;
};

/// Recognise the four shapes a user is likely to type:
///   1. https://github.com/<o>/<r>
///   2. file:///<abs-path>
///   3. <local-path> (Windows drive / POSIX absolute / UNC) → wrapped as file:///
///   4. <owner>/<repo> github shorthand → expanded to https://github.com/...
/// First match wins; nothing else is accepted (e.g. http://, git://, ssh://
/// not supported in v1.0).
std::expected<AddTarget, std::string> parse_add_input(std::string_view input) {
    AddTarget t;

    if (auto gh = parse_github_url(input)) {
        t.url = std::string(input);
        if (t.url.ends_with(".git")) t.url.resize(t.url.size() - 4);
        t.default_name = gh->repo;
        return t;
    }

    if (auto fp = parse_file_url(input)) {
        t.url = std::string(input);
        t.default_name = basename_of(fp->string());
        return t;
    }

    if (looks_like_local_path(input)) {
        std::string norm(input);
        for (auto& c : norm) if (c == '\\') c = '/';
        // file:/// + drive-or-absolute. The extra slash is only needed when
        // the path doesn't start with one already (Windows drive paths).
        t.url = norm.starts_with("/") ? ("file://" + norm) : ("file:///" + norm);
        t.default_name = basename_of(input);
        return t;
    }

    // <owner>/<repo> shorthand: exactly one '/', no scheme separator,
    // both halves non-empty.
    auto slash = input.find('/');
    if (slash != std::string_view::npos &&
        input.find('/', slash + 1) == std::string_view::npos &&
        input.find(':') == std::string_view::npos &&
        slash > 0 && slash + 1 < input.size()) {
        std::string repo(input.substr(slash + 1));
        if (repo.ends_with(".git")) repo.resize(repo.size() - 4);
        t.url = "https://github.com/" + std::string(input.substr(0, slash)) + "/" + repo;
        t.default_name = repo;
        return t;
    }

    return std::unexpected(
        "unsupported source `" + std::string(input) +
        "`\n  expected: <owner>/<repo>, https://github.com/<o>/<r>, file:///<abs-path>, or local path");
}

// ---- Fetch helpers -----------------------------------------------------

std::string default_ref(const std::string& explicit_ref) {
    if (!explicit_ref.empty()) return explicit_ref;
    return "main";
}

/// Hit api.github.com/.../commits/<ref> to get the commit sha. Best-effort:
/// any failure returns empty string (recorded as "unknown" in the registry).
/// We don't fail the whole `bp source add` over a missing commit field
/// because the tarball download already succeeded — losing visibility into
/// the exact commit is far less bad than refusing to register the source.
std::string fetch_github_commit_sha(const GithubRef& gh, const std::string& ref) {
    std::string api =
        "https://api.github.com/repos/" + gh.owner + "/" + gh.repo +
        "/commits/" + ref;
    auto body = luban::download::fetch_text(api, 15);
    if (!body) return {};
    try {
        auto j = nlohmann::json::parse(*body);
        if (j.contains("sha") && j["sha"].is_string()) {
            return j["sha"].get<std::string>();
        }
    } catch (const nlohmann::json::parse_error&) {
        // fall through
    }
    return {};
}

/// Download `<url>/archive/<ref>.zip`, extract into `target_dir`, removing
/// any prior content first so re-fetch is idempotent. archive::extract
/// auto-flattens the single `<repo>-<ref>/` top-level directory.
std::expected<void, std::string> fetch_github_tarball(
    const GithubRef& gh, const std::string& ref, const fs::path& target_dir) {
    std::string url =
        "https://github.com/" + gh.owner + "/" + gh.repo + "/archive/" + ref + ".zip";

    // Stage in cache so a partial download never lands inside the source dir.
    fs::path cache = paths::downloads_dir();
    std::error_code ec;
    fs::create_directories(cache, ec);
    fs::path zip_tmp =
        cache / ("bp_source-" + gh.owner + "-" + gh.repo + "-" + ref + ".zip");

    luban::download::DownloadOptions dlopts;
    dlopts.label = "fetch " + gh.owner + "/" + gh.repo + "@" + ref;
    auto dl = luban::download::download(url, zip_tmp, dlopts);
    if (!dl) {
        return std::unexpected("download " + url + ": " + dl.error().message);
    }

    // Wipe + recreate the target dir so old content from the previous
    // commit can't bleed through (e.g. blueprint deleted upstream still
    // visible locally). The registry holds the URL, so a destructive
    // re-fetch here is safe.
    fs::remove_all(target_dir, ec);
    fs::create_directories(target_dir, ec);
    auto ex = luban::archive::extract(zip_tmp, target_dir);
    fs::remove(zip_tmp, ec);
    if (!ex) {
        return std::unexpected("extract " + zip_tmp.string() + ": " + ex.error().message);
    }
    return {};
}

bool source_dir_looks_valid(const fs::path& dir) {
    std::error_code ec;
    return fs::is_directory(dir / "blueprints", ec);
}

// ---- Trust prompt ------------------------------------------------------

bool prompt_trust(const std::string& name, const std::string& url) {
    std::printf("\n");
    std::printf("Add bp source `%s`:\n", name.c_str());
    std::printf("  url: %s\n", url.c_str());
    std::printf("\n");
    std::printf("Blueprints from this source can run Lua extensions and\n");
    std::printf("post-install scripts. Proceed only if you trust the maintainer.\n");
    std::printf("\n");
    std::printf("Continue? [y/N] ");
    std::fflush(stdout);
    std::string line;
    if (!std::getline(std::cin, line)) return false;  // EOF = decline
    for (char& c : line) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return line == "y" || line == "yes";
}

// ---- `bp source add` ---------------------------------------------------

int run_source_add(const cli::ParsedArgs& args) {
    if (args.positional.empty()) {
        std::fprintf(stderr,
                     "bp source add: usage: bp src add <url-or-shorthand> [--name <name>] [--ref <ref>] [--yes]\n"
                     "  shorthand:    <owner>/<repo>           github\n"
                     "  full URL:     https://github.com/<o>/<r> | file:///<abs-path>\n"
                     "  bare path:    D:\\Projects\\foo  (Windows drive) | /home/u/foo (POSIX)\n");
        return 2;
    }
    auto target = parse_add_input(args.positional[0]);
    if (!target) {
        std::fprintf(stderr, "bp source add: %s\n", target.error().c_str());
        return 2;
    }
    std::string url = target->url;

    // Name resolution: --name overrides; otherwise default from URL.
    std::string name;
    if (auto it = args.opts.find("name"); it != args.opts.end() && !it->second.empty()) {
        name = it->second;
    } else {
        name = target->default_name;
    }

    std::string ref;
    if (auto it = args.opts.find("ref"); it != args.opts.end()) ref = it->second;

    if (!sr::is_valid_name(name)) {
        std::fprintf(stderr, "bp source add: invalid name `%s` (alphanumeric / dash / underscore only, max 64)\n",
                     name.c_str());
        return 2;
    }
    if (sr::is_reserved_name(name)) {
        std::fprintf(stderr,
                     "bp source add: derived name `%s` is reserved (embedded / local / main); "
                     "supply --name <other>\n", name.c_str());
        return 2;
    }

    auto entries = sr::read();
    if (!entries) {
        std::fprintf(stderr, "bp source add: read registry: %s\n", entries.error().c_str());
        return 1;
    }
    // Re-adding a name replaces in place — switching from file:// to
    // https:// (or vice-versa) is a one-command workflow, not rm + add.
    // Trust prompt still fires below because the URL may have changed.
    bool replacing = false;
    if (auto existing = sr::find(*entries, name); existing) {
        replacing = true;
        // Drop old local copy (only for non-file URLs — file:// is live-linked
        // to a directory we don't own, mirroring `run_source_rm`).
        if (!existing->url.starts_with("file://")) {
            std::error_code ec;
            fs::remove_all(paths::bp_sources_dir(name), ec);
        }
        entries->erase(std::remove_if(entries->begin(), entries->end(),
                                      [&](const sr::SourceEntry& e) { return e.name == name; }),
                       entries->end());
    }

    bool autoyes = args.flags.count("yes") && args.flags.at("yes");
    if (!autoyes && !prompt_trust(name, url)) {
        std::printf("aborted.\n");
        return 1;
    }

    sr::SourceEntry entry;
    entry.name = name;
    entry.url = url;
    entry.ref = ref;
    entry.added_at = luban::iso_time::now();

    if (auto gh = parse_github_url(url)) {
        std::string the_ref = default_ref(ref);
        fs::path target_dir = paths::bp_sources_dir(name);
        if (auto r = fetch_github_tarball(*gh, the_ref, target_dir); !r) {
            std::fprintf(stderr, "bp source add: %s\n", r.error().c_str());
            return 1;
        }
        if (!source_dir_looks_valid(target_dir)) {
            std::fprintf(stderr,
                         "bp source add: warning — fetched %s but no `blueprints/` dir at root\n",
                         target_dir.string().c_str());
            // Not a hard error; user might be planning to populate it.
        }
        std::string sha = fetch_github_commit_sha(*gh, the_ref);
        entry.commit = sha.empty() ? ("tarball:" + entry.added_at) : sha;
    } else if (auto file_path = parse_file_url(url)) {
        // file:// — passthrough; just verify the path exists and has the
        // expected layout. We don't copy: editing the source repo's .lua
        // is immediately visible to `bp apply`.
        std::error_code ec;
        if (!fs::is_directory(*file_path, ec)) {
            std::fprintf(stderr, "bp source add: file:// path is not a directory: %s\n",
                         file_path->string().c_str());
            return 1;
        }
        if (!source_dir_looks_valid(*file_path)) {
            std::fprintf(stderr,
                         "bp source add: %s has no `blueprints/` subdir (expected layout per DESIGN §9.10.2)\n",
                         file_path->string().c_str());
            return 1;
        }
        entry.commit = "local:" + entry.added_at;
    } else {
        // Should not be reachable — parse_add_input already accepts/rejects.
        std::fprintf(stderr, "bp source add: internal: post-parse url shape unknown: %s\n", url.c_str());
        return 1;
    }

    entries->push_back(entry);
    if (auto w = sr::write(*entries); !w) {
        std::fprintf(stderr, "bp source add: write registry: %s\n", w.error().c_str());
        return 1;
    }
    std::printf("%s bp source `%s` -> %s\n",
                replacing ? "updated" : "registered",
                name.c_str(), url.c_str());
    return 0;
}

// ---- `bp source update` -------------------------------------------------

int run_source_update(const cli::ParsedArgs& args) {
    auto entries = sr::read();
    if (!entries) {
        std::fprintf(stderr, "bp source update: read registry: %s\n", entries.error().c_str());
        return 1;
    }
    if (entries->empty()) {
        std::printf("(no bp sources registered)\n");
        return 0;
    }

    std::vector<sr::SourceEntry*> targets;
    if (args.positional.empty()) {
        for (auto& e : *entries) targets.push_back(&e);
    } else {
        std::string name = args.positional[0];
        for (auto& e : *entries) if (e.name == name) targets.push_back(&e);
        if (targets.empty()) {
            std::fprintf(stderr, "bp source update: no source named `%s`\n", name.c_str());
            return 1;
        }
    }

    int failures = 0;
    for (auto* e : targets) {
        std::printf("update `%s`...\n", e->name.c_str());
        if (auto gh = parse_github_url(e->url)) {
            std::string the_ref = default_ref(e->ref);
            fs::path target = paths::bp_sources_dir(e->name);
            if (auto r = fetch_github_tarball(*gh, the_ref, target); !r) {
                std::fprintf(stderr, "  %s\n", r.error().c_str());
                ++failures;
                continue;
            }
            std::string sha = fetch_github_commit_sha(*gh, the_ref);
            if (!sha.empty()) e->commit = sha;
            else e->commit = "tarball:" + luban::iso_time::now();
        } else if (auto fp = parse_file_url(e->url)) {
            // file:// is live-linked; nothing to fetch. Refresh the timestamp
            // so `ls` reflects when the user last asked for an update.
            e->commit = "local:" + luban::iso_time::now();
        } else {
            std::fprintf(stderr, "  unrecognised url shape in registry: %s\n", e->url.c_str());
            ++failures;
        }
    }

    if (auto w = sr::write(*entries); !w) {
        std::fprintf(stderr, "bp source update: write registry: %s\n", w.error().c_str());
        return 1;
    }
    return failures == 0 ? 0 : 1;
}

// ---- `bp source` dispatch ----------------------------------------------

int run_source_dispatch(const cli::ParsedArgs& args) {
    if (args.positional.empty()) {
        std::fprintf(stderr, "luban bp source: missing subcommand (add | update)\n");
        return 2;
    }
    std::string sub = args.positional[0];
    cli::ParsedArgs rest = args;
    rest.positional.erase(rest.positional.begin());

    if (sub == "add")    return run_source_add(rest);
    if (sub == "update") return run_source_update(rest);

    std::fprintf(stderr, "luban bp source: unknown subcommand `%s` (add | update)\n", sub.c_str());
    return 2;
}

// ---- `bp list` ------------------------------------------------------

bool icontains(std::string_view hay, std::string_view needle) {
    if (needle.empty()) return true;
    if (needle.size() > hay.size()) return false;
    auto tolower = [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            if (tolower(hay[i + j]) != tolower(needle[j])) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

void list_blueprints_in(const fs::path& dir, std::string_view label,
                        std::string_view pattern, int& hit_count) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    bool printed_header = false;
    for (auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        auto ext = entry.path().extension().string();
        if (ext != ".lua" && ext != ".toml") continue;
        std::string stem = entry.path().stem().string();
        if (!icontains(stem, pattern)) continue;
        if (!printed_header) {
            std::printf("%s:\n", std::string(label).c_str());
            printed_header = true;
        }
        std::printf("  %s\n", stem.c_str());
        ++hit_count;
    }
}

}  // namespace

int run_bp_source(const cli::ParsedArgs& args) {
    return run_source_dispatch(args);
}

int run_bp_list(const cli::ParsedArgs& args) {
    // Optional positional acts as a substring filter; no arg = list everything.
    std::string pattern;
    if (!args.positional.empty()) pattern = args.positional[0];
    int hits = 0;

    list_blueprints_in(paths::config_dir() / "blueprints",
                       "user-local (" + (paths::config_dir() / "blueprints").string() + ")",
                       pattern, hits);

    auto entries = sr::read();
    if (!entries) {
        std::fprintf(stderr, "bp list: read registry: %s\n", entries.error().c_str());
        return 1;
    }
    for (auto& e : *entries) {
        fs::path bp_dir;
        if (auto fp = parse_file_url(e.url)) {
            bp_dir = *fp / "blueprints";
        } else {
            bp_dir = paths::bp_sources_dir(e.name) / "blueprints";
        }
        list_blueprints_in(bp_dir, e.name + " (" + e.url + ")", pattern, hits);
    }

    if (hits == 0) {
        if (pattern.empty()) std::printf("(no blueprints found)\n");
        else std::printf("(no blueprints matching `%s`)\n", pattern.c_str());
    }
    return 0;
}

}  // namespace luban::commands
