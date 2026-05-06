// `blueprint_lua` — parses a Lua blueprint file (`return { ... }`) into a
// BlueprintSpec.
//
// A Lua blueprint is a Lua expression that evaluates to a table with the
// same shape as the TOML schema (DESIGN.md §9.1). The advantage over TOML
// is full scripting power: `if luban.platform.os() == "windows" then ...`,
// `for each in pairs(...) do ... end`, `require("./helpers")`. The
// disadvantage is requiring users to learn (a tiny bit of) Lua.
//
// We use src/lua_engine.cpp's sandboxed VM to execute the file; this
// header just walks the resulting Lua table via the lua_State API and
// fills in a BlueprintSpec.

#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

#include "blueprint.hpp"

namespace luban::lua { class Engine; }

namespace luban::blueprint_lua {

/// Parse a Lua blueprint file. Returns BlueprintSpec on success, or an
/// error string on failure. Errors include I/O problems, Lua execution
/// failures (sandbox violation, syntax error, runtime error), and
/// schema-validation failures (return value is not a table, missing
/// required `name`, etc.).
///
/// This overload spins a fresh Engine for the parse and discards it on
/// return. Use this when the blueprint never calls
/// `luban.register_renderer` (the bp's TOML side, tests, or any path
/// where the parse engine doesn't need to outlive parse).
[[nodiscard]] std::expected<luban::blueprint::BlueprintSpec, std::string>
parse_file(const std::filesystem::path& path);

/// Same but parse from a string (used by tests).
[[nodiscard]] std::expected<luban::blueprint::BlueprintSpec, std::string>
parse_string(std::string_view content);

/// Parse using a caller-owned Engine. The Engine MUST stay alive across
/// the subsequent render phase, because any `luban.register_renderer`
/// calls deposit luaL_refs into THIS engine's LUA_REGISTRYINDEX — those
/// refs become invalid the moment the engine destructs.
///
/// Used by blueprint_apply for Lua-form bps so registered renderers
/// remain callable when config_renderer hits `[config.X]` blocks later.
[[nodiscard]] std::expected<luban::blueprint::BlueprintSpec, std::string>
parse_file_in_engine(luban::lua::Engine& engine,
                     const std::filesystem::path& path);

[[nodiscard]] std::expected<luban::blueprint::BlueprintSpec, std::string>
parse_string_in_engine(luban::lua::Engine& engine, std::string_view content);

}  // namespace luban::blueprint_lua
