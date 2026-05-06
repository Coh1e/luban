// `curl_subprocess` — Win32 download backend that drives `curl.exe` as a
// subprocess instead of going through WinHTTP.
//
// Why: WinHTTP's protocol-version logic kept fighting GitHub's CDN tier:
//   v0.1.4 default-on HTTP/3 → 10s QUIC handshake stalls on UDP-throttled networks
//   v0.2.2 H3 opt-in, H2 default → fine for codeload.github.com
//   v0.2.6 H1.1 default       → because objects.githubusercontent.com (Fastly)
//                                served WinHTTP h2 at 14.9 KiB/s vs h1.1 at 47 MiB/s
//                                on the same machine and same URL
//
// Each fix had a per-host cliff. WinHTTP's protocol-flag option is per-session,
// not per-host, so we can't pick the right protocol per request without
// keeping multiple sessions and routing manually. Easier to outsource the
// HTTP stack entirely. curl.exe ships with Win10 1803+ in System32; install.ps1
// already uses it; it negotiates h2/h3/h1.1 per host correctly without us
// touching options. Subprocess overhead (~10ms spawn + ~50ms re-read for
// SHA) is invisible against ~1s typical small download / ~5s+ large.
//
// POSIX builds keep using libcurl in-process (system-shared lib, no binary
// cost); only Win32 changes here.

#pragma once

#ifndef _WIN32
#error "curl_subprocess is Win32-only — POSIX uses libcurl in-process"
#endif

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "hash.hpp"
#include "progress.hpp"

namespace luban::curl_subprocess {

namespace fs = std::filesystem;

enum class ErrorKind {
    NotFound,        ///< curl.exe not located in System32 or PATH
    SpawnFailed,     ///< CreateProcessW failed (rare — would mean OS issue)
    Network,         ///< curl exited with a network error code (6/7/28/56/...)
    HttpClient,      ///< 4xx response (don't retry)
    HttpServer,      ///< 5xx response (do retry)
    Stalled,         ///< Stall watchdog tripped (rolling 1 KiB/s floor over 15s)
    Io,              ///< Filesystem operation failed (couldn't open dest, ...)
};

struct Error {
    ErrorKind kind;
    std::string message;
    int curl_exit = 0;  ///< Populated for Network / HttpClient / HttpServer
};

struct Result {
    int64_t bytes;          ///< Bytes received over the wire (post-resume offset)
    int64_t final_size;     ///< Final size of dest on disk (== existing + bytes)
};

struct Options {
    int connect_timeout_seconds = 30;
    int max_time_seconds = 900;       ///< Wall clock cap; covers the whole transfer
    bool resume = true;               ///< -C - so retries pick up where last ended
    luban::progress::Bar* progress = nullptr;  ///< Optional; drives a polling thread
};

/// Stall-watchdog window. Public for tests.
constexpr int kStallWindowSeconds = 15;
constexpr std::int64_t kStallMinBytesPerSec = 1024;  // 1 KiB/s

/// Locate curl.exe. Tries `<System32>\curl.exe` first (Win10 1803+ baseline),
/// then PATH search. Caches the result. Returns empty path if not found.
[[nodiscard]] fs::path find_curl_exe();

/// Download `url` into `dest`. Single attempt; caller does retry/backoff.
/// Resume via `-C -` is automatic when `dest` already exists from a prior
/// failed attempt (curl Range request from the existing offset; GitHub's
/// S3-backed release CDN honors Range).
///
/// On success: returns Result with bytes + final_size. dest is the on-disk
/// file (caller still does verify_and_promote → SHA + rename → final path).
///
/// On failure: returns Error. Caller maps to retry / bail by ErrorKind.
[[nodiscard]] std::expected<Result, Error> download_to_file(
    const std::string& url, const fs::path& dest, const Options& opts);

/// Fetch `url` body into a string. For small JSON / commit-sha responses;
/// not for large file transfers (pulls into memory).
[[nodiscard]] std::expected<std::string, Error> fetch_text(
    const std::string& url, int timeout_seconds);

/// Issue a HEAD request and return Content-Length (or nullopt if absent /
/// chunked / probe failed). ~50ms wall-clock cost on a healthy connection;
/// callers use it to seed the progress bar with a real total so the live
/// frame can show percentage. Optional — when nullopt, callers fall back
/// to "running byte count + rate" rendering.
[[nodiscard]] std::optional<std::int64_t> head_content_length(
    const std::string& url, int timeout_seconds = 30);

}  // namespace luban::curl_subprocess
