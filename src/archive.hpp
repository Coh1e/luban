#pragma once
// 1:1 port of luban_boot/archive.py 的 zip 子集。
// tar.gz / tar.xz / 7z 暂不支持（Python 端尚靠系统 tar；M3 跨平台时再补）。
//
// 安全：
// - 拒绝包含 ".." / 绝对路径 的 entry（路径穿越防护）
// - 拒绝 .7z / .exe（self-extracting installer）
// - 单顶层目录自动扁平化（archive/foo-1.0/...→ dest/...）

#include <expected>
#include <filesystem>
#include <string>

namespace luban::archive {

namespace fs = std::filesystem;

enum class ErrorKind {
    Unsupported,           // 7z / msi / nsis / .exe 自解压
    UnsafeEntry,           // 路径穿越
    Io,                    // 文件操作失败
    Corrupt,               // ZIP 头不合法
};

struct Error {
    ErrorKind kind;
    std::string message;
};

// 把 archive 解压到 dest_dir（递归创建）。dest_dir 内容会被填充。
// 如果 archive 顶层是单一目录，扁平化（`archive/cmake-4.3.2/bin/...` →
// `dest_dir/bin/...`），与 Python 行为一致。
std::expected<void, Error> extract(const fs::path& archive_path, const fs::path& dest_dir);

}  // namespace luban::archive
