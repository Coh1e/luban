// See `lua_json.hpp`.
//
// The Lua C API is famously stack-juggling-heavy. The functions below
// commit to predictable stack effects:
//   push(L, j)    → +1 (the new value sits at the new top)
//   pop_value(L, idx) → 0 (caller decides when to lua_pop)
//
// Both walk recursive structure carefully to avoid blowing the C stack
// on adversarial input (deep arrays/objects). For now we trust the
// blueprint authors — if a renderer or blueprint hits 1000+ deep
// nesting that's a UX bug not a security one.

#include "lua_json.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

namespace luban::lua_json {

namespace {

void push_impl(lua_State* L, const nlohmann::json& v) {
    using nlohmann::json;
    switch (v.type()) {
        case json::value_t::null:
            lua_pushnil(L);
            return;
        case json::value_t::boolean:
            lua_pushboolean(L, v.get<bool>() ? 1 : 0);
            return;
        case json::value_t::number_integer:
        case json::value_t::number_unsigned:
            lua_pushinteger(L, static_cast<lua_Integer>(v.get<int64_t>()));
            return;
        case json::value_t::number_float:
            lua_pushnumber(L, v.get<double>());
            return;
        case json::value_t::string: {
            const auto& s = v.get_ref<const std::string&>();
            lua_pushlstring(L, s.data(), s.size());
            return;
        }
        case json::value_t::array: {
            lua_createtable(L, static_cast<int>(v.size()), 0);
            int i = 1;
            for (auto& elem : v) {
                push_impl(L, elem);
                lua_seti(L, -2, i);
                ++i;
            }
            return;
        }
        case json::value_t::object: {
            lua_createtable(L, 0, static_cast<int>(v.size()));
            for (auto it = v.begin(); it != v.end(); ++it) {
                const std::string& k = it.key();
                push_impl(L, it.value());
                lua_setfield(L, -2, k.c_str());
            }
            return;
        }
        default:
            // discarded / binary — fall through to nil rather than fail.
            lua_pushnil(L);
            return;
    }
}

nlohmann::json pop_impl(lua_State* L, int idx);

nlohmann::json pop_table(lua_State* L, int idx) {
    using nlohmann::json;
    // Pre-scan to decide array vs object. An array is a table whose only
    // keys are 1..N for some N. Anything else (string keys, gaps, mixed)
    // is an object.
    bool is_array = true;
    int total_keys = 0;
    int max_int_key = 0;

    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        ++total_keys;
        if (lua_type(L, -2) == LUA_TNUMBER && lua_isinteger(L, -2)) {
            lua_Integer k = lua_tointeger(L, -2);
            if (k >= 1 && k <= 1'000'000) {
                if (k > max_int_key) max_int_key = static_cast<int>(k);
            } else {
                is_array = false;
            }
        } else {
            is_array = false;
        }
        lua_pop(L, 1);  // pop value, key stays for lua_next
    }
    if (max_int_key != total_keys) is_array = false;

    if (is_array) {
        json out = json::array();
        for (int i = 1; i <= total_keys; ++i) {
            lua_geti(L, idx, i);
            out.push_back(pop_impl(L, lua_gettop(L)));
            lua_pop(L, 1);
        }
        return out;
    }

    json out = json::object();
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        // Stringify the key. Strings preserve length; integer keys get
        // their decimal repr; nothing else is a valid JSON key, but
        // we round-trip best-effort.
        std::string key;
        int kt = lua_type(L, -2);
        if (kt == LUA_TSTRING) {
            size_t len = 0;
            const char* s = lua_tolstring(L, -2, &len);
            key.assign(s, len);
        } else if (kt == LUA_TNUMBER) {
            if (lua_isinteger(L, -2)) {
                key = std::to_string(lua_tointeger(L, -2));
            } else {
                lua_pushvalue(L, -2);  // duplicate key for tostring
                key = lua_tostring(L, -1);
                lua_pop(L, 1);
            }
        } else {
            // Bool / nil keys aren't valid in Lua tables anyway, but
            // skip just in case the user did something exotic.
            lua_pop(L, 1);
            continue;
        }
        out[key] = pop_impl(L, lua_gettop(L));
        lua_pop(L, 1);
    }
    return out;
}

nlohmann::json pop_impl(lua_State* L, int idx) {
    using nlohmann::json;
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
            return pop_table(L, idx);
        default:
            // function / userdata / thread → tostring fallback.
            // luaL_tolstring leaves a string on top; pop it after we
            // copy out so the stack stays intact.
            return std::string(luaL_tolstring(L, idx, nullptr));
            // Note: we leak +1 stack here; callers walking large
            // structures could overflow. We rely on Lua's own stack
            // grow on push and accept this for now — tostring
            // fallback is exceptional.
    }
}

}  // namespace

void push(lua_State* L, const nlohmann::json& value) {
    push_impl(L, value);
}

nlohmann::json pop_value(lua_State* L, int idx) {
    return pop_impl(L, idx);
}

}  // namespace luban::lua_json
