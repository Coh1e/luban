#pragma once
// 生成 luban.cmake — plan §3。
// 输入：vcpkg.json (deps) + luban.toml (偏好) + targets list (动词维护)。
// 输出：luban_apply() + luban_register_targets() + find_package 调用。

#include <filesystem>
#include <string>
#include <vector>

#include "luban_toml.hpp"
#include "vcpkg_manifest.hpp"

namespace luban::luban_cmake_gen {

namespace fs = std::filesystem;

struct GenInputs {
    luban_toml::Config toml;                        // 缺失 = 默认
    vcpkg_manifest::Manifest manifest;              // deps 来源
    std::vector<std::string> targets;               // LUBAN_TARGETS 列表
};

// 渲染整个 luban.cmake 内容（带头注释 + 函数 + 维护信息）。
std::string render(const GenInputs& in);

// 读取项目目录里的输入并写 luban.cmake 到项目根。
// 项目目录 = vcpkg.json / luban.toml / luban.cmake 所在目录。
void regenerate_in_project(const fs::path& project_dir,
                           const std::vector<std::string>& targets);

// 从已存在的 luban.cmake 中提取 LUBAN_TARGETS 列表（解析 set(LUBAN_TARGETS ...)）。
// 不存在或解析不出返回空 vector。
std::vector<std::string> read_targets_from_cmake(const fs::path& project_dir);

}  // namespace luban::luban_cmake_gen
