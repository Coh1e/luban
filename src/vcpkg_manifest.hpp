#pragma once
// vcpkg.json manifest mode 读写。
//
// schema 参考 https://learn.microsoft.com/en-us/vcpkg/reference/vcpkg-json
//
// 我们只编辑用户最常用的字段：name / version / dependencies。
// 其他字段（features / overrides / supports / builtin-baseline）原样读写、不动。

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "json.hpp"

namespace luban::vcpkg_manifest {

namespace fs = std::filesystem;

struct Dependency {
    std::string name;
    // 可选版本约束。Empty = 无约束（取 baseline）。
    // 形如 "10" / "10.2" / "10.2.1"
    std::optional<std::string> version_ge;
    // 未来可加 features / platform / etc.
};

struct Manifest {
    std::string name = "myapp";
    std::string version = "0.1.0";
    std::vector<Dependency> dependencies;
    // 保留未识别的顶层字段，写出时合并回去
    nlohmann::json extras;       // 不动
};

// 读 vcpkg.json。文件不存在返回默认 Manifest（name 用 fallback_name）。
Manifest load(const fs::path& path, const std::string& fallback_name = "myapp");

// 原子写入（tmp + rename）。保留 `extras` 字段。
void save(const fs::path& path, const Manifest& m);

// 添加 dep（幂等，重复 add 替换原 entry）。
// `version_ge` 例 "10" → vcpkg 的 `version>=`。空字符串则不加版本约束。
void add(Manifest& m, const std::string& pkg, const std::string& version_ge = "");

// 删 dep。返回是否真删了。
bool remove(Manifest& m, const std::string& pkg);

// 解析 "fmt@10" / "fmt@10.2.1" / "fmt" → (name, version_ge)
std::pair<std::string, std::string> parse_pkg_spec(const std::string& spec);

}  // namespace luban::vcpkg_manifest
