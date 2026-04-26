#pragma once
// luban.toml 读 — schema v1（plan §7）。
// 缺失文件 / 缺失字段 = 全默认。整个 luban.toml 是可选的，用户没偏好就别建。

#include <filesystem>
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

struct Config {
    ProjectSection project;
    ScaffoldSection scaffold;
};

// 从 path 读 TOML；不存在或解析失败返回默认 Config（不抛）。
Config load(const fs::path& path);

// 解析单字符串：从内存 TOML 文本读。便于测试。
Config load_from_text(const std::string& text);

}  // namespace luban::luban_toml
