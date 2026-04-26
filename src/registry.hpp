#pragma once
// 1:1 schema with luban_boot/registry.py — installed.json is shared with
// Python during the migration. Schema=1.

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace luban::registry {

namespace fs = std::filesystem;

struct ComponentRecord {
    std::string name;
    std::string version;
    std::string source;
    std::string url;
    std::string hash_spec;        // "sha256:<hex>"
    std::vector<std::string> store_keys;
    std::string toolchain_dir;    // e.g. "cmake-4.3.2-x86_64"
    std::vector<std::pair<std::string, std::string>> bins;  // (alias, rel_path)
    std::string architecture = "x86_64";
    std::string installed_at;
};

// Load <state>/installed.json. Empty map if missing or unparseable.
std::map<std::string, ComponentRecord> load_installed();

// Atomic write — tmp + rename, like Python.
void save_installed(const std::map<std::string, ComponentRecord>& records);

struct AliasHit {
    std::string component;        // 来自哪个 ComponentRecord
    std::string alias;            // 命中的 alias
    fs::path exe;                 // 解析后的绝对路径
    std::vector<std::string> all_components;  // 多组件冲突时全部命中的列表
};

// 在 installed.json 里找 alias。同名 alias 多组件时取**第一个**，all_components 全列。
// 找不到返回 nullopt。
std::optional<AliasHit> resolve_alias(const std::string& alias);

}  // namespace luban::registry
