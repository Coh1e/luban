// See `source_resolver.hpp`.
//
// This TU contains the network-free half: scheme parsing, inline
// pass-through, and the dispatch in resolve(). The github-scheme
// implementation lives in source_resolver_github.cpp (split out so
// luban-tests can link a thin variant without dragging in WinHTTP /
// libcurl / nlohmann::json transitively).
//
// The two halves are wired together via a function-pointer registry
// (set at static-init time by source_resolver_github.cpp). Tests
// don't link that TU, so the pointer stays null and `github:` returns
// a "not available in this build" error — fine because nothing in the
// test suite calls resolve() with a github source.

#include "source_resolver.hpp"

namespace luban::source_resolver {

namespace detail {

using GithubResolverFn =
    std::expected<luban::blueprint_lock::LockedTool, std::string> (*)(
        const luban::blueprint::ToolSpec&);

namespace {
GithubResolverFn g_github_fn = nullptr;
}  // namespace

void set_github_resolver(GithubResolverFn fn) { g_github_fn = fn; }

GithubResolverFn github_resolver() { return g_github_fn; }

}  // namespace detail

namespace {

namespace bp = luban::blueprint;
namespace bpl = luban::blueprint_lock;

bpl::LockedTool inline_passthrough(const bp::ToolSpec& spec) {
    bpl::LockedTool out;
    out.version = spec.version.value_or("");
    out.source = spec.source.value_or("");
    for (auto& p : spec.platforms) {
        bpl::LockedPlatform lp;
        lp.url = p.url;
        lp.sha256 = p.sha256;
        lp.bin = p.bin;
        // artifact_id will be filled in by the store module after it
        // computes the canonical input hash. Leave empty here so an
        // empty value distinguishes "not yet stored" from "stored".
        lp.artifact_id = p.artifact_id;
        out.platforms.emplace(p.target, std::move(lp));
    }
    return out;
}

}  // namespace

std::string source_scheme(std::string_view source) {
    auto colon = source.find(':');
    if (colon == std::string_view::npos) return "";
    return std::string(source.substr(0, colon));
}

std::string source_body(std::string_view source) {
    auto colon = source.find(':');
    if (colon == std::string_view::npos) return "";
    return std::string(source.substr(colon + 1));
}

std::expected<bpl::LockedTool, std::string> resolve(const bp::ToolSpec& spec) {
    // Mode 1: inline platforms — always wins regardless of source value.
    // Users who write platform blocks have full control; we don't second-
    // guess them by also hitting GitHub.
    if (!spec.platforms.empty()) {
        return inline_passthrough(spec);
    }

    // Mode 2: auto-resolve via source scheme.
    if (!spec.source.has_value()) {
        return std::unexpected("tool `" + spec.name +
                               "` has neither `source` nor inline platforms");
    }

    const std::string& src = *spec.source;
    std::string scheme = source_scheme(src);

    if (scheme == "github") {
        if (auto fn = detail::github_resolver()) return fn(spec);
        return std::unexpected(
            "github: source resolver not linked in this build (test builds "
            "exclude it; production luban.exe should always have it)");
    }

    if (scheme.empty()) {
        return std::unexpected("tool `" + spec.name + "`: malformed source `" +
                               src + "` (expected scheme:body, e.g. github:owner/repo)");
    }

    return std::unexpected("tool `" + spec.name + "`: unknown source scheme `" +
                           scheme + "` (supported: github)");
}

}  // namespace luban::source_resolver
