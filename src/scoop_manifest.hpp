#pragma once
// Scoop manifest 解析 — 1:1 port of luban_boot/scoop_manifest.py.
// 只采 url+hash+extract_dir+bin 元数据；installer.script / pre_install /
// post_install / uninstaller / persist / psmodule 这 6 个字段一律拒绝
// (UnsafeManifest)，需要 overlay 才能放行。

#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "json.hpp"

namespace luban::scoop_manifest {

class UnsafeManifest : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class IncompleteManifest : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct BinEntry {
    std::string relative_path;        // archive 内 exe 的相对路径
    std::string alias;                // CLI 别名
    std::vector<std::string> prefix_args;
};

struct ResolvedManifest {
    std::string name;
    std::string version;
    std::string url;
    std::string hash_spec;            // "<algo>:<hex>"
    std::optional<std::string> extract_dir;
    std::optional<std::string> extract_to;
    std::vector<BinEntry> bins;
    std::map<std::string, std::string> env_set;
    std::vector<std::string> env_add_path;
    std::vector<std::string> depends;
    std::string architecture = "x86_64";
};

// 从已 parse 好的 JSON 树提取——`name` 用于错误信息，`arch` 选 architecture 段
// 的目标 (常用 "x86_64"，也支持 "x86" / "aarch64")。
ResolvedManifest parse(const nlohmann::json& manifest,
                       const std::string& name,
                       const std::string& arch = "x86_64");

}  // namespace luban::scoop_manifest
