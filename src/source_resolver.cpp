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

#include <string_view>

#include "platform.hpp"
#include "resolver_registry.hpp"
#include "store.hpp"

namespace luban::source_resolver {

namespace detail {

using GithubResolverFn =
    std::expected<luban::blueprint_lock::LockedTool, std::string> (*)(
        const luban::blueprint::ToolSpec&);

namespace {
GithubResolverFn g_github_fn = nullptr;
GithubResolverFn g_pwsh_fn   = nullptr;  // pwsh-module: scheme
}  // namespace

void set_github_resolver(GithubResolverFn fn) { g_github_fn = fn; }
void set_pwsh_module_resolver(GithubResolverFn fn) { g_pwsh_fn = fn; }

GithubResolverFn github_resolver()      { return g_github_fn; }
GithubResolverFn pwsh_module_resolver() { return g_pwsh_fn; }

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
    return resolve_with_registry(spec, nullptr);
}

namespace {

// Wrap a registered ResolverFn's LockedPlatform into a full LockedTool
// (version + source + target triplet + artifact_id), defaulting bin and
// artifact_id when the callback didn't pre-populate them. Identical to
// the post-resolve fix-up the legacy invoke_lua_resolver did before AH.
std::expected<bpl::LockedTool, std::string> finalize_native_resolved(
    bpl::LockedPlatform plat, const bp::ToolSpec& spec) {
    bpl::LockedTool out;
    out.version = spec.version.value_or("");
    out.source = spec.source.value_or("");
    std::string target = std::string(luban::platform::host_triplet());
    plat.bin = plat.bin.empty() ? spec.bin.value_or(spec.name + ".exe") : plat.bin;
    plat.artifact_id = luban::store::compute_artifact_id(
        spec.name, out.version, target, plat.sha256);
    out.platforms.emplace(target, std::move(plat));
    return out;
}

}  // namespace

std::expected<bpl::LockedTool, std::string> resolve_with_registry(
    const bp::ToolSpec& spec,
    const ::luban::resolver_registry::ResolverRegistry* registry) {
    // Mode 1: inline platforms — always wins regardless of source value.
    if (!spec.platforms.empty()) {
        return inline_passthrough(spec);
    }

    if (!spec.source.has_value()) {
        return std::unexpected("tool `" + spec.name +
                               "` has neither `source` nor inline platforms");
    }

    const std::string& src = *spec.source;
    std::string scheme = source_scheme(src);

    // Mode 2a: bp-registered scheme via registry — checked FIRST so a bp
    // can shadow a built-in scheme if it really wants to (DESIGN §9.9
    // "同一注册表" — same registry, last-wins). AH boundary: pure callback
    // dispatch via find_native; this TU never sees lua_State*.
    if (registry) {
        if (auto* fn = registry->find_native(scheme)) {
            auto plat = (*fn)(spec);
            if (!plat) return std::unexpected(plat.error());
            return finalize_native_resolved(std::move(*plat), spec);
        }
    }

    // Mode 2b: built-in scheme dispatch.
    if (scheme == "github") {
        if (auto fn = detail::github_resolver()) return fn(spec);
        return std::unexpected(
            "github: source resolver not linked in this build (test builds "
            "exclude it; production luban.exe should always have it)");
    }

    if (scheme == "pwsh-module") {
        if (auto fn = detail::pwsh_module_resolver()) return fn(spec);
        return std::unexpected(
            "pwsh-module: source resolver not linked in this build");
    }

    if (scheme.empty()) {
        return std::unexpected("tool `" + spec.name + "`: malformed source `" +
                               src + "` (expected scheme:body, e.g. github:owner/repo)");
    }

    return std::unexpected("tool `" + spec.name + "`: unknown source scheme `" +
                           scheme + "` (supported: github, pwsh-module" +
                           (registry && !registry->empty() ? ", + bp-registered" : "") +
                           ")");
}

}  // namespace luban::source_resolver
