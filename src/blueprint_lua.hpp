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

namespace luban::blueprint_lua {

/// Parse a Lua blueprint file. Returns BlueprintSpec on success, or an
/// error string on failure. Errors include I/O problems, Lua execution
/// failures (sandbox violation, syntax error, runtime error), and
/// schema-validation failures (return value is not a table, missing
/// required `name`, etc.).
[[nodiscard]] std::expected<luban::blueprint::BlueprintSpec, std::string>
parse_file(const std::filesystem::path& path);

/// Same but parse from a string (used by tests).
[[nodiscard]] std::expected<luban::blueprint::BlueprintSpec, std::string>
parse_string(std::string_view content);

}  // namespace luban::blueprint_lua
