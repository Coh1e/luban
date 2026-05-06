#pragma once
// luban.toml reader — schema v1 (plan §7).
// Missing file / missing field = full defaults. The whole luban.toml is
// optional; if a user has no preferences, they shouldn't need to create it.

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace luban::luban_toml {

namespace fs = std::filesystem;

struct ProjectSection {
    std::string default_preset = "default";
    std::string triplet = "x64-mingw-static";
    int cpp = 23;
};

enum class WarningLevel { Off, Normal, Strict };

struct ScaffoldSection {
    WarningLevel warnings = WarningLevel::Normal;
    std::vector<std::string> sanitizers;     // e.g. ["address","ub"]
};

// Per-project toolchain version pins (OQ-2). Maps component name (as it
// appears in installed.json — "cmake" / "ninja" / "llvm-mingw" / etc.) to
// a required version string. `luban build` checks pins against the
// registry on launch and warns on mismatch (it doesn't auto-install or
// hard-fail by default — that escalation is left to a future flag).
//
// Empty map = no pins, no checks. Symmetric with [scaffold].sanitizers
// being optional.
//
// Format in luban.toml:
//
//     [toolchain]
//     cmake = "4.3.2"
//     ninja = "1.13.2"
//     llvm-mingw = "20260421"
//
// Versions are matched as raw strings (no semver range parsing in v1).
// vcpkg / cmake / ninja release tags are mostly date- or dotted-int form
// already; range support can come later if a real ask emerges.
using ToolchainPins = std::map<std::string, std::string>;

// Per-project vcpkg port → cmake target hint (DESIGN §14.2, 议题 AB).
// Lets users teach luban about ports that aren't in the built-in
// lib_targets table (or override the built-in mapping). Consulted by
// luban_cmake_gen before falling back to lib_targets::lookup.
//
// Format in luban.toml:
//
//     [ports.mylib]
//     find_package = "MyLib"        # find_package(MyLib CONFIG REQUIRED)
//     targets = ["MyLib::mylib"]    # target_link_libraries(... PRIVATE MyLib::mylib)
//
// Either field may be omitted: missing find_package defaults to the port
// name verbatim; missing targets means luban emits the find_package line
// without auto-link (user wires target_link_libraries manually).
struct PortHint {
    std::string find_package;
    std::vector<std::string> targets;
};
using PortHints = std::map<std::string, PortHint>;

struct Config {
    ProjectSection project;
    ScaffoldSection scaffold;
    ToolchainPins toolchain;
    PortHints ports;
};

// Read TOML from `path`. Missing file or parse error returns a default
// Config (never throws); the parse error is logged at warn level.
Config load(const fs::path& path);

// Parse TOML from an in-memory string. Used by tests so they don't need
// to round-trip through the filesystem.
Config load_from_text(const std::string& text);

}  // namespace luban::luban_toml
