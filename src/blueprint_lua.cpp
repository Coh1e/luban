// See `blueprint_lua.hpp`.
//
// Implementation: spin a fresh sandboxed lua_engine::Engine, load the
// chunk, expect it to return a single table, then walk that table via
// the Lua C API to populate BlueprintSpec. This is "Lua read-back"
// territory: lua_pcall has already run user code, the result sits at
// stack top, and we drill into it lua_getfield by lua_getfield.
//
// Why we don't use luaL_dostring + lua_State directly here: the engine
// already installs the sandbox + luban.* API. Inlining that would
// duplicate code and risk drift. Engine::eval_file leaves the result on
// the stack as a side effect of the JS_Eval-shaped contract, but here
// we need finer access — so we go via Engine::state() and run our own
// luaL_loadbuffer + lua_pcall in the same VM.

#include "blueprint_lua.hpp"

#include <fstream>
#include <sstream>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include "lua_engine.hpp"

namespace luban::blueprint_lua {

namespace {

namespace bp = luban::blueprint;
using nlohmann::json;

// ---- Lua table → nlohmann::json conversion -----------------------------
//
// Walks a Lua table at stack index `idx` and emits it as JSON. Used for
// the body of `config.X` blocks. Same conversion philosophy as TOML's
// node_to_json: anything we can't represent natively becomes a string
// via tostring.

json value_to_json(lua_State* L, int idx);

json table_to_json(lua_State* L, int idx) {
    // First pass: detect array-vs-object by scanning integer keys 1..N.
    // If the table has a contiguous run of integer keys starting at 1
    // and no other keys, treat it as an array; otherwise object.
    bool is_array = true;
    int max_idx = 0;
    int total_keys = 0;
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        ++total_keys;
        if (lua_type(L, -2) == LUA_TNUMBER && lua_isinteger(L, -2)) {
            lua_Integer k = lua_tointeger(L, -2);
            if (k >= 1 && k <= 1'000'000) {
                if (k > max_idx) max_idx = static_cast<int>(k);
            } else {
                is_array = false;
            }
        } else {
            is_array = false;
        }
        lua_pop(L, 1);  // pop value, keep key for lua_next
    }
    if (max_idx != total_keys) is_array = false;

    if (is_array) {
        json out = json::array();
        out.get_ptr<json::array_t*>()->reserve(total_keys);
        for (int i = 1; i <= total_keys; ++i) {
            lua_geti(L, idx, i);
            out.push_back(value_to_json(L, lua_gettop(L)));
            lua_pop(L, 1);
        }
        return out;
    }

    json out = json::object();
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        // Lua key types we accept as JSON keys: string + integer.
        // Integers are stringified (matches what TOML does too).
        std::string key;
        if (lua_type(L, -2) == LUA_TSTRING) {
            size_t len = 0;
            const char* s = lua_tolstring(L, -2, &len);
            key.assign(s, len);
        } else if (lua_type(L, -2) == LUA_TNUMBER) {
            // Stringify number key — happens when users index by number.
            key = std::to_string(lua_tointeger(L, -2));
        } else {
            // Unknown key type — skip silently. Renderers won't see it.
            lua_pop(L, 1);
            continue;
        }
        // value is now at top of stack (idx -1).
        out[key] = value_to_json(L, lua_gettop(L));
        lua_pop(L, 1);
    }
    return out;
}

json value_to_json(lua_State* L, int idx) {
    int t = lua_type(L, idx);
    switch (t) {
        case LUA_TNIL:
            return nullptr;
        case LUA_TBOOLEAN:
            return static_cast<bool>(lua_toboolean(L, idx));
        case LUA_TNUMBER:
            if (lua_isinteger(L, idx)) {
                return static_cast<int64_t>(lua_tointeger(L, idx));
            }
            return lua_tonumber(L, idx);
        case LUA_TSTRING: {
            size_t len = 0;
            const char* s = lua_tolstring(L, idx, &len);
            return std::string(s, len);
        }
        case LUA_TTABLE:
            return table_to_json(L, idx);
        default:
            // function / userdata / thread: stringify as best we can.
            return std::string(luaL_tolstring(L, idx, nullptr));
    }
}

// ---- BlueprintSpec extractor -------------------------------------------

struct ParseCtx {
    bp::BlueprintSpec spec;
    std::vector<std::string> errors;
    void err(std::string m) { errors.push_back(std::move(m)); }
    bool ok() const { return errors.empty(); }
    std::string joined() const {
        std::string out;
        for (auto& e : errors) {
            out += e;
            out += "\n";
        }
        if (!out.empty()) out.pop_back();
        return out;
    }
};

// Read a string field at result_idx.<field_name>; returns nullopt if
// absent or wrong type. Keeps stack discipline (always +0 net).
std::optional<std::string> opt_str_field(lua_State* L, int idx, const char* field) {
    lua_getfield(L, idx, field);
    std::optional<std::string> out;
    if (lua_isstring(L, -1)) {
        size_t len = 0;
        const char* s = lua_tolstring(L, -1, &len);
        out = std::string(s, len);
    }
    lua_pop(L, 1);
    return out;
}

// Lua-idiomatic schema is plural (`tools = { ... }`); the TOML schema is
// singular (`[tool.X]`) and a few legacy bps mirrored TOML in their Lua
// form too. Try plural first (matches CLAUDE.md), fall back to singular.
// Returns true with the table on the stack; caller pops.
bool collection_field(lua_State* L, int idx, const char* plural,
                      const char* singular) {
    lua_getfield(L, idx, plural);
    if (lua_type(L, -1) == LUA_TTABLE) return true;
    lua_pop(L, 1);
    lua_getfield(L, idx, singular);
    return lua_type(L, -1) == LUA_TTABLE;
}

void parse_tools(lua_State* L, int blueprint_idx, ParseCtx& ctx) {
    if (!collection_field(L, blueprint_idx, "tools", "tool")) {
        lua_pop(L, 1);
        return;
    }
    int tools_idx = lua_gettop(L);

    // Iterate { ripgrep = {...}, fd = {...} }
    lua_pushnil(L);
    while (lua_next(L, tools_idx) != 0) {
        // key at -2, value at -1
        if (lua_type(L, -2) != LUA_TSTRING || !lua_istable(L, -1)) {
            lua_pop(L, 1);
            continue;
        }
        std::string tool_name = lua_tostring(L, -2);
        int tool_idx = lua_gettop(L);

        bp::ToolSpec tool;
        tool.name = tool_name;
        if (auto v = opt_str_field(L, tool_idx, "source"))        tool.source        = *v;
        if (auto v = opt_str_field(L, tool_idx, "version"))       tool.version       = *v;
        if (auto v = opt_str_field(L, tool_idx, "bin"))           tool.bin           = *v;
        if (auto v = opt_str_field(L, tool_idx, "post_install"))  tool.post_install  = *v;
        if (auto v = opt_str_field(L, tool_idx, "external_skip")) tool.external_skip = *v;
        if (auto v = opt_str_field(L, tool_idx, "shim_dir"))      tool.shim_dir      = *v;

        // no_shim — boolean, optional, default false.
        lua_getfield(L, tool_idx, "no_shim");
        if (lua_isboolean(L, -1)) tool.no_shim = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);

        // shims = { "bin/a.exe", "bin/b.exe" } — string array of explicit
        // shim paths relative to the artifact root. Composes with shim_dir.
        lua_getfield(L, tool_idx, "shims");
        if (lua_istable(L, -1)) {
            int shims_idx = lua_gettop(L);
            int n = static_cast<int>(luaL_len(L, shims_idx));
            for (int i = 1; i <= n; ++i) {
                lua_geti(L, shims_idx, i);
                if (lua_type(L, -1) == LUA_TSTRING) {
                    size_t len = 0;
                    const char* s = lua_tolstring(L, -1, &len);
                    tool.shims.emplace_back(s, len);
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);  // pop shims

        // platforms = { { target=, url=, sha256=, bin= }, ... }
        lua_getfield(L, tool_idx, "platforms");
        if (lua_istable(L, -1)) {
            int plats_idx = lua_gettop(L);
            // Walk array indices 1..N
            int n = static_cast<int>(luaL_len(L, plats_idx));
            for (int i = 1; i <= n; ++i) {
                lua_geti(L, plats_idx, i);
                if (lua_istable(L, -1)) {
                    int p_idx = lua_gettop(L);
                    bp::PlatformSpec ps;
                    if (auto v = opt_str_field(L, p_idx, "target")) ps.target = *v;
                    if (auto v = opt_str_field(L, p_idx, "url"))    ps.url    = *v;
                    if (auto v = opt_str_field(L, p_idx, "sha256")) ps.sha256 = *v;
                    if (auto v = opt_str_field(L, p_idx, "bin"))    ps.bin    = *v;

                    if (ps.target.empty()) ctx.err("tools." + tool_name + ".platforms[" + std::to_string(i) + "] missing `target`");
                    if (ps.url.empty())    ctx.err("tools." + tool_name + ".platforms[" + std::to_string(i) + "] missing `url`");
                    if (ps.sha256.empty()) ctx.err("tools." + tool_name + ".platforms[" + std::to_string(i) + "] missing `sha256`");
                    tool.platforms.push_back(std::move(ps));
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);  // pop platforms table

        if (!tool.source.has_value() && tool.platforms.empty()) {
            ctx.err("tools." + tool_name + " needs either `source` or `platforms`");
        }

        ctx.spec.tools.push_back(std::move(tool));
        lua_pop(L, 1);  // pop tool table; key remains for lua_next
    }
    lua_pop(L, 1);  // pop tools table
}

void parse_configs(lua_State* L, int blueprint_idx, ParseCtx& ctx) {
    if (!collection_field(L, blueprint_idx, "configs", "config")) {
        lua_pop(L, 1);
        return;
    }
    int cfgs_idx = lua_gettop(L);

    lua_pushnil(L);
    while (lua_next(L, cfgs_idx) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING || !lua_istable(L, -1)) {
            lua_pop(L, 1);
            continue;
        }
        bp::ConfigSpec cfg;
        cfg.name = lua_tostring(L, -2);
        int cfg_idx = lua_gettop(L);
        if (auto v = opt_str_field(L, cfg_idx, "for_tool")) cfg.for_tool = *v;
        // Build JSON from the cfg table — for_tool is a parser-level
        // field, so it leaks into the JSON view too. That's harmless
        // (renderers ignore unknown fields), unlike TOML where we
        // stripped it. Symmetry with TOML side optional; future cleanup.
        cfg.config = table_to_json(L, cfg_idx);
        ctx.spec.configs.push_back(std::move(cfg));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

void parse_files(lua_State* L, int blueprint_idx, ParseCtx& ctx) {
    if (!collection_field(L, blueprint_idx, "files", "file")) {
        lua_pop(L, 1);
        return;
    }
    int files_idx = lua_gettop(L);

    lua_pushnil(L);
    while (lua_next(L, files_idx) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING || !lua_istable(L, -1)) {
            lua_pop(L, 1);
            continue;
        }
        std::string target = lua_tostring(L, -2);
        int f_idx = lua_gettop(L);
        bp::FileSpec f;
        f.target_path = target;
        if (auto v = opt_str_field(L, f_idx, "content")) f.content = *v;

        std::string mode = "replace";
        if (auto v = opt_str_field(L, f_idx, "mode")) mode = *v;
        if (mode == "replace") {
            f.mode = bp::FileMode::Replace;
        } else if (mode == "drop-in" || mode == "dropin") {
            f.mode = bp::FileMode::DropIn;
        } else if (mode == "merge") {
            f.mode = bp::FileMode::Merge;
        } else if (mode == "append") {
            f.mode = bp::FileMode::Append;
        } else {
            ctx.err("file[\"" + target + "\"] unknown mode `" + mode +
                    "` (expected: replace | drop-in | merge | append)");
            lua_pop(L, 1);
            continue;
        }
        if (f.content.empty()) {
            ctx.err("file[\"" + target + "\"] missing required `content`");
            lua_pop(L, 1);
            continue;
        }
        ctx.spec.files.push_back(std::move(f));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

void parse_meta(lua_State* L, int blueprint_idx, ParseCtx& ctx) {
    lua_getfield(L, blueprint_idx, "meta");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    int meta_idx = lua_gettop(L);

    auto walk_str_array = [&](const char* field, std::vector<std::string>& out) {
        lua_getfield(L, meta_idx, field);
        if (lua_istable(L, -1)) {
            int n = static_cast<int>(luaL_len(L, -1));
            for (int i = 1; i <= n; ++i) {
                lua_geti(L, -1, i);
                if (lua_isstring(L, -1)) {
                    size_t len = 0;
                    const char* s = lua_tolstring(L, -1, &len);
                    out.emplace_back(s, len);
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    };
    walk_str_array("requires", ctx.spec.meta.requires_);
    walk_str_array("conflicts", ctx.spec.meta.conflicts);

    lua_pop(L, 1);  // pop meta table
}

std::expected<bp::BlueprintSpec, std::string> walk_top(lua_State* L) {
    if (!lua_istable(L, -1)) {
        return std::unexpected("blueprint must `return { ... }` (a table)");
    }
    int idx = lua_gettop(L);

    ParseCtx ctx;
    if (auto v = opt_str_field(L, idx, "name")) {
        ctx.spec.name = *v;
    } else {
        ctx.err("missing required top-level `name`");
    }
    if (auto v = opt_str_field(L, idx, "description")) ctx.spec.description = *v;

    // Optional `schema` field (defaults to 1).
    lua_getfield(L, idx, "schema");
    if (lua_isinteger(L, -1)) {
        ctx.spec.schema = static_cast<int>(lua_tointeger(L, -1));
    }
    lua_pop(L, 1);

    parse_tools(L, idx, ctx);
    parse_configs(L, idx, ctx);
    parse_files(L, idx, ctx);
    parse_meta(L, idx, ctx);

    if (!ctx.ok()) return std::unexpected(ctx.joined());
    return std::move(ctx.spec);
}

// Drive the load + run + walk against `L`. Caller owns the engine; on
// return the bp's `return { ... }` table has been popped and (if any)
// register_renderer side effects have landed in whatever registry was
// attached to L's LUA_REGISTRYINDEX.
std::expected<bp::BlueprintSpec, std::string> exec_and_walk_on(
    lua_State* L, std::string_view code, const char* chunkname) {
    if (luaL_loadbuffer(L, code.data(), code.size(), chunkname) != LUA_OK) {
        std::string err = lua_tostring(L, -1);
        lua_pop(L, 1);
        return std::unexpected("Lua syntax: " + err);
    }
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        std::string err = lua_tostring(L, -1);
        lua_pop(L, 1);
        return std::unexpected("Lua runtime: " + err);
    }
    auto result = walk_top(L);
    lua_pop(L, 1);
    return result;
}

std::expected<bp::BlueprintSpec, std::string> exec_and_walk(
    std::string_view code, const char* chunkname) {
    luban::lua::Engine engine;
    return exec_and_walk_on(engine.state(), code, chunkname);
}

}  // namespace

std::expected<bp::BlueprintSpec, std::string> parse_file(
    const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
        return std::unexpected("cannot open " + path.string());
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string filename = path.string();
    return exec_and_walk(ss.str(), filename.c_str());
}

std::expected<bp::BlueprintSpec, std::string> parse_string(
    std::string_view content) {
    return exec_and_walk(content, "=blueprint_lua_string");
}

std::expected<bp::BlueprintSpec, std::string> parse_file_in_engine(
    luban::lua::Engine& engine, const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
        return std::unexpected("cannot open " + path.string());
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string filename = path.string();
    return exec_and_walk_on(engine.state(), ss.str(), filename.c_str());
}

std::expected<bp::BlueprintSpec, std::string> parse_string_in_engine(
    luban::lua::Engine& engine, std::string_view content) {
    return exec_and_walk_on(engine.state(), content, "=blueprint_lua_string");
}

}  // namespace luban::blueprint_lua
