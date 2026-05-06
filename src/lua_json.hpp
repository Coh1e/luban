// `lua_json` — small bridge between nlohmann::json and Lua's C API stack.
//
// Used in two places:
//   - `blueprint_lua.cpp` — pop the user's `return { ... }` table and
//     turn its [programs.X] sub-tables into nlohmann::json so renderers
//     can consume them uniformly.
//   - `program_renderer.cpp` — push a json cfg + ctx onto the Lua stack
//     so a Lua renderer module's M.render(cfg, ctx) can read structured
//     data without a custom protocol.
//
// Conversion conventions:
//   - JSON object → Lua table with string keys
//   - JSON array → Lua table with 1-based integer keys (Lua-idiomatic)
//   - JSON null → Lua nil (lossy on the way back; pop_json reads nil as
//     json::null)
//   - JSON int64 → Lua integer; JSON float → Lua number
//   - JSON string → Lua string (length-preserving, NUL-safe)
//
// Lua-side, when popping back to JSON we apply the same array-vs-object
// heuristic as blueprint_lua had: contiguous integer keys 1..N → array;
// anything else → object. This lets users author Lua tables with
// arbitrary key order and still serialize predictably.

#pragma once

#include <string>

#include "json.hpp"

struct lua_State;

namespace luban::lua_json {

/// Push a JSON value onto Lua's stack as a Lua value. Stack effect: +1.
/// On allocation failure inside Lua, this raises a Lua error from the
/// engine — callers must invoke under lua_pcall protection.
void push(lua_State* L, const nlohmann::json& value);

/// Read the Lua value at stack position `idx` and return its JSON
/// equivalent. Stack is unchanged. Functions / userdata / threads
/// stringify via tostring (best-effort). Numbers preserve int vs float
/// distinction.
[[nodiscard]] nlohmann::json pop_value(lua_State* L, int idx);

}  // namespace luban::lua_json
