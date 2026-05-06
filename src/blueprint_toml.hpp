// `blueprint_toml` — parses a TOML blueprint file into a BlueprintSpec.
//
// The TOML form is the static-data path: best for blueprints that don't
// need conditionals, computed values, or imports. For those features
// users write Lua (blueprint_lua.cpp) or JS (blueprint_qjs.cpp) instead.
//
// Schema reference: docs/DESIGN.md §9.2.

#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

#include "blueprint.hpp"

namespace luban::blueprint_toml {

/// Parse a TOML blueprint file. Returns the populated BlueprintSpec on
/// success or an error string on failure. Errors include I/O problems,
/// TOML syntax errors, and schema-validation errors (missing required
/// `name` field, unknown `mode` value, etc.).
[[nodiscard]] std::expected<luban::blueprint::BlueprintSpec, std::string>
parse_file(const std::filesystem::path& path);

/// Same but parse from a string (used by tests).
[[nodiscard]] std::expected<luban::blueprint::BlueprintSpec, std::string>
parse_string(std::string_view content);

}  // namespace luban::blueprint_toml
