// `source_resolver_github` — the network half of source_resolver. Held
// in a separate TU so luban-tests can link a network-free build of the
// resolver. See source_resolver.cpp's header comment for rationale.
//
// What this file does:
//   1. parse `github:owner/repo` into (owner, repo).
//   2. GET https://api.github.com/repos/<owner>/<repo>/releases/latest
//      via download::fetch_text (anonymous; 60 req/h is plenty).
//   3. score release assets against the host triplet via a name-based
//      heuristic (see score_asset_for_target).
//   4. download the best-scoring asset to compute sha256.
//   5. emit a LockedTool with one LockedPlatform for the host triplet.
//
// Source-zip fallback (microsoft/vcpkg-style repos):
//   When a release has zero `assets:` attachments, fall back to the
//   release's `zipball_url` (GitHub's auto-generated source archive).
//   These don't carry a published checksum so we always download to
//   compute sha256. archive::extract flattens the single top-level
//   `<repo>-<tag>/` dir, so the resulting store_dir contents are
//   identical to a hand-crafted source archive. Pair with a `bin`
//   override + `post_install` (e.g. `bootstrap-vcpkg.bat`) to get
//   a working tool out of source.
//
// What this file does NOT do:
//   - try multiple targets (only host triplet; users on other hosts
//     re-run apply on those hosts to lock per-host).
//   - parse semver / pre-release tags (we trust GitHub's "latest").
//   - support GITHUB_TOKEN auth (deferred until rate-limited users surface).

#include "source_resolver.hpp"

#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>

#include "json.hpp"

#include "download.hpp"
#include "hash.hpp"
#include "paths.hpp"
#include "platform.hpp"
#include "store.hpp"

namespace luban::source_resolver {

namespace {

namespace bp = luban::blueprint;
namespace bpl = luban::blueprint_lock;
namespace fs = std::filesystem;

bool contains_ci(std::string_view hay, std::string_view needle) {
    if (needle.size() > hay.size()) return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            char a = static_cast<char>(std::tolower(static_cast<unsigned char>(hay[i + j])));
            char b = static_cast<char>(std::tolower(static_cast<unsigned char>(needle[j])));
            if (a != b) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

// ---- target-specific asset scorers --------------------------------------
// Each scorer returns a higher score for assets that look like they belong
// to that target. Negative-scoring substrings let us strongly disqualify
// "linux" assets when looking for windows (and vice-versa).

// Cheap "OS-token in filename" detector used by all three scorers.
// We tested looser substring matches like contains_ci(name, "win") and
// contains_ci(name, "mac") and they collide with arbitrary release
// names (`darwin` contains "win", `macroscope-1.0.zip` contains "mac"),
// so we anchor on a separator on either side: '-', '_', '.', '/', or
// EOL. Result: `ninja-win.zip` and `ninja-mac.zip` no longer score
// identically as we head into a windows-x64 host search.
bool has_os_token(std::string_view name, std::initializer_list<std::string_view> tokens) {
    auto sep = [](char c) {
        return c == '-' || c == '_' || c == '.' || c == '/';
    };
    std::string lower(name);
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (auto t : tokens) {
        std::string needle(t);
        for (auto& c : needle) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        size_t pos = 0;
        while ((pos = lower.find(needle, pos)) != std::string::npos) {
            bool left  = (pos == 0)                || sep(lower[pos - 1]);
            bool right = (pos + needle.size() >= lower.size())
                       || sep(lower[pos + needle.size()]);
            if (left && right) return true;
            ++pos;
        }
    }
    return false;
}

int score_for_windows_x64(std::string_view name) {
    int s = 0;
    if (contains_ci(name, "x86_64") || contains_ci(name, "amd64") ||
        contains_ci(name, "x64") || contains_ci(name, "win64")) s += 5;
    // Windows OS tokens — accept "windows" / "msvc" / "pc-windows"
    // as substring (they're long enough to be unambiguous), plus
    // anchored "win" so `ninja-win.zip` scores correctly without
    // also rewarding `darwin`.
    if (contains_ci(name, "windows") || contains_ci(name, "msvc") ||
        contains_ci(name, "pc-windows") ||
        has_os_token(name, {"win"})) s += 5;
    if (contains_ci(name, ".zip")) s += 2;
    if (contains_ci(name, "musl")) s -= 5;
    if (contains_ci(name, "arm64") || contains_ci(name, "aarch64") ||
        contains_ci(name, "armv7")) s -= 10;
    if (contains_ci(name, "darwin") || contains_ci(name, "apple") ||
        contains_ci(name, "macos") || contains_ci(name, "osx") ||
        has_os_token(name, {"mac"})) s -= 10;
    if (contains_ci(name, "linux") && !contains_ci(name, "windows") &&
        !contains_ci(name, "msvc")) s -= 10;
    return s;
}

int score_for_linux_x64(std::string_view name) {
    int s = 0;
    if (contains_ci(name, "x86_64") || contains_ci(name, "amd64") ||
        contains_ci(name, "x64")) s += 5;
    if (contains_ci(name, "linux")) s += 5;
    if (contains_ci(name, ".tar.gz") || contains_ci(name, ".tgz")) s += 2;
    if (contains_ci(name, "unknown-linux-gnu")) s += 1;
    if (contains_ci(name, "windows") || contains_ci(name, "msvc") ||
        has_os_token(name, {"win"})) s -= 10;
    if (contains_ci(name, "darwin") || contains_ci(name, "macos") ||
        has_os_token(name, {"mac"})) s -= 10;
    if (contains_ci(name, "arm64") || contains_ci(name, "aarch64")) s -= 10;
    return s;
}

int score_for_macos(std::string_view name, bool arm) {
    int s = 0;
    if (contains_ci(name, "darwin") || contains_ci(name, "apple") ||
        contains_ci(name, "macos") || contains_ci(name, "osx") ||
        has_os_token(name, {"mac"})) s += 5;
    if (arm) {
        if (contains_ci(name, "arm64") || contains_ci(name, "aarch64")) s += 5;
        if (contains_ci(name, "x86_64") && !contains_ci(name, "arm")) s -= 2;
    } else {
        if (contains_ci(name, "x86_64") || contains_ci(name, "x64")) s += 5;
        if (contains_ci(name, "arm64") || contains_ci(name, "aarch64")) s -= 5;
    }
    if (contains_ci(name, ".tar.gz") || contains_ci(name, ".zip")) s += 2;
    if (contains_ci(name, "windows") || contains_ci(name, "msvc") ||
        has_os_token(name, {"win"})) s -= 10;
    if (contains_ci(name, "linux")) s -= 10;
    return s;
}

int score_asset_for_target(std::string_view asset_name, std::string_view target) {
    if (target == "windows-x64") return score_for_windows_x64(asset_name);
    if (target == "linux-x64")   return score_for_linux_x64(asset_name);
    if (target == "macos-arm64") return score_for_macos(asset_name, true);
    if (target == "macos-x64")   return score_for_macos(asset_name, false);
    return 0;  // Unknown target — no preference.
}

struct GhRef { std::string owner, repo; };

std::optional<GhRef> parse_github_source(std::string_view src) {
    auto colon = src.find(':');
    if (colon == std::string_view::npos) return std::nullopt;
    if (src.substr(0, colon) != "github") return std::nullopt;
    auto rest = src.substr(colon + 1);
    auto slash = rest.find('/');
    if (slash == std::string_view::npos) return std::nullopt;
    GhRef out;
    out.owner = std::string(rest.substr(0, slash));
    out.repo = std::string(rest.substr(slash + 1));
    if (out.owner.empty() || out.repo.empty()) return std::nullopt;
    return out;
}

std::string default_bin_name(const bp::ToolSpec& spec, std::string_view target) {
    bool is_windows = target.find("windows") != std::string_view::npos;
    if (spec.bin.has_value() && !spec.bin->empty()) {
        std::string b = *spec.bin;
        if (is_windows && b.find(".exe") == std::string::npos) b += ".exe";
        return b;
    }
    std::string b = spec.name;
    if (is_windows) b += ".exe";
    return b;
}

std::expected<bpl::LockedTool, std::string> resolve_github_impl(
    const bp::ToolSpec& spec) {
    auto gh = parse_github_source(*spec.source);
    if (!gh) {
        return std::unexpected("malformed github source `" + *spec.source +
                               "` (expected github:owner/repo)");
    }

    std::string api_url = "https://api.github.com/repos/" + gh->owner + "/" +
                          gh->repo + "/releases/latest";
    auto body = luban::download::fetch_text(api_url, 30);
    if (!body) {
        return std::unexpected("github API GET " + api_url + ": " + body.error().message);
    }

    nlohmann::json release;
    try {
        release = nlohmann::json::parse(*body);
    } catch (const nlohmann::json::parse_error& e) {
        return std::unexpected(std::string("github API JSON parse: ") + e.what());
    }

    bpl::LockedTool out;
    out.source = *spec.source;
    if (release.contains("tag_name") && release["tag_name"].is_string()) {
        std::string tag = release["tag_name"].get<std::string>();
        if (!tag.empty() && tag[0] == 'v') tag.erase(0, 1);
        out.version = tag;
    }

    std::string target = std::string(luban::platform::host_triplet());

    // Source-zip fallback path: kicks in when the release has zero
    // attachments (microsoft/vcpkg, many pure-source-archive repos).
    // Uses the API-provided zipball_url (GitHub's auto-generated source
    // .zip), which has no published checksum, so we always download to
    // compute sha256. The extractor flattens the single top-level
    // `<repo>-<tag>/` dir on its own.
    bool no_assets = !release.contains("assets") ||
                     !release["assets"].is_array() ||
                     release["assets"].empty();
    if (no_assets) {
        if (!release.contains("zipball_url") ||
            !release["zipball_url"].is_string()) {
            return std::unexpected("github release for " + gh->owner + "/" +
                                   gh->repo + " has neither `assets` nor `zipball_url`");
        }
        std::string zip_url = release["zipball_url"].get<std::string>();

        bpl::LockedPlatform lp;
        lp.url = zip_url;
        lp.bin = default_bin_name(spec, target);

        // No digest path — source archives never advertise sha256. Always
        // download to hash. This is bigger than a release asset (full
        // source tree), but only on first resolve / `bp update`.
        fs::path cache = paths::cache_dir() / "downloads";
        std::error_code ec;
        fs::create_directories(cache, ec);
        fs::path tmp = cache / (spec.name + "-resolve-source.zip");

        luban::download::DownloadOptions dlopts;
        dlopts.label = "resolve " + spec.name + " (source-zip fallback)";
        auto dl = luban::download::download(zip_url, tmp, dlopts);
        if (!dl) {
            return std::unexpected("download " + zip_url + ": " + dl.error().message);
        }
        lp.sha256 = "sha256:" + dl->sha256.hex;
        fs::remove(tmp, ec);
        lp.artifact_id = luban::store::compute_artifact_id(
            spec.name, out.version, target, lp.sha256);

        out.platforms.emplace(target, std::move(lp));
        return out;
    }

    auto& assets = release["assets"];

    int best_score = 0;
    std::string best_url;
    std::string best_name;
    std::string best_digest;  ///< "sha256:..." from the API if present.
    for (auto& asset : assets) {
        if (!asset.is_object()) continue;
        if (!asset.contains("name") || !asset["name"].is_string()) continue;
        std::string name = asset["name"].get<std::string>();
        // Skip checksum / signature files.
        if (contains_ci(name, ".sha256") || contains_ci(name, ".asc") ||
            contains_ci(name, ".sig") || contains_ci(name, "checksums")) continue;

        int s = score_asset_for_target(name, target);
        if (s > best_score) {
            best_score = s;
            best_name = name;
            best_digest.clear();
            if (asset.contains("browser_download_url") &&
                asset["browser_download_url"].is_string()) {
                best_url = asset["browser_download_url"].get<std::string>();
            }
            // Newer GitHub releases (2025+) populate `digest` with
            // "sha256:..." for assets that have a published checksum.
            // When present we can skip the resolve-time download
            // entirely — store::fetch will re-verify when it
            // downloads anyway.
            if (asset.contains("digest") && asset["digest"].is_string()) {
                std::string d = asset["digest"].get<std::string>();
                if (d.starts_with("sha256:") && d.size() == 7 + 64) {
                    best_digest = d;
                }
            }
        }
    }

    if (best_score == 0 || best_url.empty()) {
        return std::unexpected("no matching asset for `" + spec.name +
                               "` on " + target + " among " +
                               std::to_string(assets.size()) +
                               " release assets in " + gh->owner + "/" + gh->repo);
    }

    bpl::LockedPlatform lp;
    lp.url = best_url;
    lp.bin = default_bin_name(spec, target);

    if (!best_digest.empty()) {
        // Fast path: GitHub gave us the digest. No download needed.
        lp.sha256 = best_digest;
    } else {
        // Slow path: download the asset to compute sha256. ~5-30 MB
        // per tool, but only on first resolve / `luban bp update`.
        // store::fetch will re-download + re-verify when apply
        // actually installs, so this temp file is throwaway.
        fs::path cache = paths::cache_dir() / "downloads";
        std::error_code ec;
        fs::create_directories(cache, ec);
        fs::path tmp = cache / (spec.name + "-resolve-" + best_name);

        luban::download::DownloadOptions dlopts;
        dlopts.label = "resolve " + spec.name;
        auto dl = luban::download::download(best_url, tmp, dlopts);
        if (!dl) {
            return std::unexpected("download " + best_url + ": " + dl.error().message);
        }
        lp.sha256 = "sha256:" + dl->sha256.hex;
        fs::remove(tmp, ec);
    }

    // Compute artifact_id NOW that sha256 is final. Earlier comment
    // ("store will fill on first fetch") was aspirational — store::fetch
    // takes artifact_id by value and uses it as-is, so writing it empty
    // resulted in `<store>/.archive` cache file + final_dir == store root,
    // which then made fs::rename fail with "system cannot find the path
    // specified". Compute here so the lock is self-contained.
    lp.artifact_id = luban::store::compute_artifact_id(
        spec.name, out.version, target, lp.sha256);

    out.platforms.emplace(target, std::move(lp));
    return out;
}

// Static initializer to wire resolve_github_impl into source_resolver's
// dispatch. Tests don't link this TU, so resolve_github_impl is absent
// and the `github:` scheme returns "not available in this build."
struct Registrar {
    Registrar() {
        luban::source_resolver::detail::set_github_resolver(&resolve_github_impl);
    }
};
[[maybe_unused]] Registrar g_registrar;

}  // namespace

}  // namespace luban::source_resolver
