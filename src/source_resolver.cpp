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

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

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

// Walk a Lua table at the top of the stack, populating a LockedPlatform.
// Expected fields (string keys): url, sha256, bin. Missing fields fall
// through to default behaviour at the call site.
std::expected<bpl::LockedPlatform, std::string> walk_resolver_result(
    lua_State* L, std::string_view tool_name) {
    if (!lua_istable(L, -1)) {
        std::string actual = lua_typename(L, lua_type(L, -1));
        return std::unexpected("registered resolver for `" +
                               std::string(tool_name) +
                               "` returned " + actual + ", expected table");
    }
    bpl::LockedPlatform lp;
    lua_getfield(L, -1, "url");
    if (lua_isstring(L, -1)) lp.url = lua_tostring(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, -1, "sha256");
    if (lua_isstring(L, -1)) lp.sha256 = lua_tostring(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, -1, "bin");
    if (lua_isstring(L, -1)) lp.bin = lua_tostring(L, -1);
    lua_pop(L, 1);
    if (lp.url.empty()) {
        return std::unexpected("registered resolver for `" +
                               std::string(tool_name) +
                               "` returned table without required `url` field");
    }
    return lp;
}

// Invoke a bp-registered Lua resolver: push fn from registry ref, push
// a Lua-side `spec` table {name, source, version}, pcall, walk result.
std::expected<bpl::LockedTool, std::string> invoke_lua_resolver(
    const ::luban::resolver_registry::Entry& entry,
    const bp::ToolSpec& spec) {
    lua_State* L = entry.L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, entry.fn_ref);
    if (!lua_isfunction(L, -1)) {
        std::string actual = lua_typename(L, lua_type(L, -1));
        lua_pop(L, 1);
        return std::unexpected(
            "registered resolver ref for `" + spec.name + "` is " + actual +
            ", expected function (engine recycled?)");
    }
    // Build spec table.
    lua_newtable(L);
    lua_pushstring(L, spec.name.c_str());
    lua_setfield(L, -2, "name");
    if (spec.source) {
        lua_pushstring(L, spec.source->c_str());
        lua_setfield(L, -2, "source");
    }
    if (spec.version) {
        lua_pushstring(L, spec.version->c_str());
        lua_setfield(L, -2, "version");
    }
    if (spec.bin) {
        lua_pushstring(L, spec.bin->c_str());
        lua_setfield(L, -2, "bin");
    }
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        std::string err = lua_tostring(L, -1);
        lua_pop(L, 1);
        return std::unexpected("registered resolver for `" + spec.name +
                               "` raised: " + err);
    }
    auto plat = walk_resolver_result(L, spec.name);
    lua_pop(L, 1);  // pop result table
    if (!plat) return std::unexpected(plat.error());

    bpl::LockedTool out;
    out.version = spec.version.value_or("");
    out.source = spec.source.value_or("");
    std::string target = std::string(luban::platform::host_triplet());
    plat->bin = plat->bin.empty() ? spec.bin.value_or(spec.name + ".exe") : plat->bin;
    plat->artifact_id = luban::store::compute_artifact_id(
        spec.name, out.version, target, plat->sha256);
    out.platforms.emplace(target, std::move(*plat));
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
    // "同一注册表" — same registry, last-wins).
    if (registry) {
        if (auto entry = registry->find(scheme)) {
            return invoke_lua_resolver(*entry, spec);
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
