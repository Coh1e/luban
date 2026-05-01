#pragma once
// luban.toml 读 — schema v1（plan §7）。
// 缺失文件 / 缺失字段 = 全默认。整个 luban.toml 是可选的，用户没偏好就别建。

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
    std::vector<std::string> sanitizers;     // 例 ["address","ub"]
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

struct Config {
    ProjectSection project;
    ScaffoldSection scaffold;
    ToolchainPins toolchain;
};

// 从 path 读 TOML；不存在或解析失败返回默认 Config（不抛）。
Config load(const fs::path& path);

// 解析单字符串：从内存 TOML 文本读。便于测试。
Config load_from_text(const std::string& text);

}  // namespace luban::luban_toml
