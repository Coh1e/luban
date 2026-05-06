#pragma once
// 文件哈希 — Win32 BCrypt（系统自带，Win10+）。
// 1:1 port of luban_boot/download.py:HashSpec / hash_file 的功能子集。
// 用途：M2 setup 下载校验、`luban add` 后期 vendor cache 校验。

#include <filesystem>
#include <optional>
#include <string>

namespace luban::hash {

namespace fs = std::filesystem;

enum class Algorithm { Sha256, Sha512, Md5, Sha1 };

struct HashSpec {
    Algorithm algo;
    std::string hex;        // 小写
};

std::string algo_name(Algorithm a);
std::optional<Algorithm> parse_algo(std::string_view name);

// 解析 "sha256:<hex>" / "sha512:<hex>" / 仅 hex（默认 sha256）。
// 不合法返回 nullopt。
std::optional<HashSpec> parse(std::string_view raw);

// 串行化为 "<algo>:<hex>"。
std::string to_string(const HashSpec& spec);

// 计算文件哈希。OOM / 不存在 / 读错 → nullopt。
std::optional<HashSpec> hash_file(const fs::path& path, Algorithm algo = Algorithm::Sha256);

// 验证文件 SHA == 期望值（大小写不敏感）。
bool verify_file(const fs::path& path, const HashSpec& expected);

}  // namespace luban::hash
