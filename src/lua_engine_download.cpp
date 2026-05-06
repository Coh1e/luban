// `luban.download` real implementation. Held in a separate TU so
// luban-tests can link a network-free Engine without dragging in
// download.cpp + WinHTTP/libcurl + miniz transitively. See
// source_resolver_github.cpp for the same pattern.

#include "lua_engine.hpp"

#include <filesystem>
#include <string>
#include <system_error>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include "download.hpp"
#include "hash.hpp"
#include "paths.hpp"

namespace luban::lua {
namespace {

// Fetch url to the content-addressed store staging dir, return a Lua
// table { path = "<abs>", sha256 = "<hex>", bytes = <int> } on success
// or (nil, "<error message>") on failure. Only https:// URLs are
// accepted (sandbox policy).
int real_download(lua_State* L) {
    const char* url_c = luaL_checkstring(L, 1);
    std::string url = url_c;
    std::string sha_in;
    if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
        sha_in = luaL_checkstring(L, 2);
    }
    if (url.rfind("https://", 0) != 0) {
        lua_pushnil(L);
        lua_pushstring(L, "luban.download: only https:// URLs are accepted in the blueprint sandbox");
        return 2;
    }

    auto staging = ::luban::paths::store_sha256_dir() / "staging";
    std::error_code ec;
    std::filesystem::create_directories(staging, ec);
    auto dest = staging / std::filesystem::path(url).filename();
    if (dest.filename().empty()) dest = staging / "blob";

    ::luban::download::DownloadOptions opts;
    if (!sha_in.empty()) {
        if (auto spec = ::luban::hash::parse(sha_in)) {
            opts.expected_hash = *spec;
        } else {
            lua_pushnil(L);
            lua_pushfstring(L, "luban.download: bad sha256 spec '%s'", sha_in.c_str());
            return 2;
        }
    }

    auto result = ::luban::download::download(url, dest, opts);
    if (!result) {
        lua_pushnil(L);
        lua_pushfstring(L, "luban.download: %s", result.error().message.c_str());
        return 2;
    }

    lua_newtable(L);
    std::string dest_s = dest.string();
    lua_pushlstring(L, dest_s.data(), dest_s.size());
    lua_setfield(L, -2, "path");
    std::string sha_s = ::luban::hash::to_string(result->sha256);
    lua_pushlstring(L, sha_s.data(), sha_s.size());
    lua_setfield(L, -2, "sha256");
    lua_pushinteger(L, static_cast<lua_Integer>(result->bytes));
    lua_setfield(L, -2, "bytes");
    return 1;
}

// Static initializer: register real_download with lua_engine.cpp's
// dispatch table. Production luban.exe links this TU; luban-tests does not.
struct Init {
    Init() { ext::set_download_fn(&real_download); }
};
const Init g_init;

}  // namespace
}  // namespace luban::lua
