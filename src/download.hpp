#pragma once
// HTTPS 下载 — Win32 WinHTTP（系统自带）。
// 1:1 port of luban_boot/download.py 的 download() / fetch_text() / hash_file()。
//
// 设计点：
// - 流式（一次 64 KiB 块，不全读入内存）
// - 同步算 sha256（流过一次哈希一次）
// - 支持期望 HashSpec 校验，不匹配抛 HashMismatch
// - 重试 3 次，指数退避（4xx 不重试）
// - tempfile + atomic rename（中断不留半文件）
// - TTY 进度条（stderr，10Hz throttle）
// - LUBAN_NO_PROGRESS=1 环境变量关进度条

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>

#include "hash.hpp"

namespace luban::download {

namespace fs = std::filesystem;

enum class ErrorKind {
    HttpClient,        // 4xx — 不重试
    HttpServer,        // 5xx
    Network,           // DNS / 连接 / 超时
    HashMismatch,      // 校验失败
    Io,                // 写文件失败
    Cancelled,
};

struct Error {
    ErrorKind kind;
    std::string message;
};

struct DownloadOptions {
    std::optional<hash::HashSpec> expected_hash;
    std::string label;                 // 进度条左侧标签，默认是 url 末段
    int retries = 3;
    int timeout_seconds = 30;
};

struct DownloadResult {
    hash::HashSpec sha256;             // 实际下完算出的 sha256（永远）
    int64_t bytes;
};

// 下载 url 到 dest，原子（tmp + rename）。
std::expected<DownloadResult, Error> download(
    const std::string& url, const fs::path& dest, const DownloadOptions& opts = {});

// 拉取小文本（manifest 等）。失败返回 nullopt（调用方决定是否记日志）。
std::expected<std::string, Error> fetch_text(
    const std::string& url, int timeout_seconds = 15);

}  // namespace luban::download
