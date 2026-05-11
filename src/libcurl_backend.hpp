// `libcurl_backend` — in-process download backend using libcurl + Schannel.
//
// Replaces the v0.3.0–v1.0.6 `curl_subprocess` driver (which spawned
// System32\curl.exe). Motivation (DESIGN §1 amendment, 2026-05-12):
//
//   1. Version locking. System32 curl varies across Windows builds; some
//      older Win10 images ship a curl that doesn't negotiate HTTP/2 well
//      against GitHub's Fastly CDN. Vendoring libcurl pins the behavior.
//   2. HTTP/2 forced. CURLOPT_HTTP_VERSION = CURL_HTTP_VERSION_2TLS makes
//      ALPN insist on h2. VN/CN network notes (see memory) show h2
//      handles per-IP throttle far better than parallel h1.1 streams.
//   3. Real-time progress. CURLOPT_XFERINFOFUNCTION fires every ~200ms
//      with the actual byte counter from libcurl's internal state — no
//      more polling `dest` file size, no more 100ms FS-stat latency.
//   4. Architecture cleanup. No spawn cost, no stderr-capture file, no
//      polling thread, no curl exit-code parsing. ~250 lines vs ~530.
//
// Schannel is the SSL backend (CURL_USE_SCHANNEL=ON in CMake). Uses the
// Windows certificate store; no CA bundle to ship; no OpenSSL vendor.
// Invariant 7 (static-linked, no DLL deps) holds: Schannel deps are
// crypt32/secur32/etc. which are part of the Windows base image.
//
// Thread safety: each call constructs its own curl_easy handle; no
// shared state. `curl_global_init` is called once via std::call_once on
// first entry, defensively — main.cpp also does an explicit init at
// startup. Either ordering is safe.

#pragma once

#ifndef _WIN32
#error "libcurl_backend is Win32-only (DESIGN §1 — luban targets Windows)"
#endif

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "hash.hpp"
#include "progress.hpp"

namespace luban::libcurl_backend {

namespace fs = std::filesystem;

enum class ErrorKind {
    Network,         ///< Connection / TLS / transport-level failure (retry helps)
    HttpClient,      ///< 4xx response — caller bails without retry
    HttpServer,      ///< 5xx response — caller retries
    Stalled,         ///< CURLOPT_LOW_SPEED_LIMIT/TIME tripped (15s @ <1 KiB/s)
    Io,              ///< Local filesystem write failed (out of space, perms)
};

struct Error {
    ErrorKind kind;
    std::string message;
    int curl_code = 0;       ///< CURLcode for transport errors; 0 otherwise
    long http_status = 0;    ///< Populated for HttpClient / HttpServer
};

struct Result {
    int64_t bytes;          ///< Bytes received over the wire on this attempt
                            ///< (post-resume offset — i.e., new bytes only).
    int64_t final_size;     ///< Final size of dest on disk after the call.
};

struct Options {
    int connect_timeout_seconds = 30;
    int max_time_seconds = 900;       ///< Wall-clock cap; covers the whole transfer
    bool resume = true;               ///< Set CURLOPT_RESUME_FROM_LARGE to existing size
    luban::progress::Bar* progress = nullptr;  ///< Optional; XFERINFOFUNCTION drives it
};

/// Stall-watchdog window. Mirrored from libcurl's CURLOPT_LOW_SPEED_*.
constexpr int kStallWindowSeconds = 15;
constexpr long kStallMinBytesPerSec = 1024;  // 1 KiB/s

/// One-shot init for libcurl globals (idempotent; std::call_once-guarded).
/// Callers don't normally invoke this — main() does it explicitly, and
/// every public function below calls it defensively too.
void ensure_global_init();

/// Download `url` into `dest`. Single attempt; caller does retry/backoff.
/// Resume: when `opts.resume && dest exists`, sets CURLOPT_RESUME_FROM_LARGE
/// to the existing file size and opens dest in append-binary mode. GitHub's
/// release CDN honors Range requests so this works across retries.
///
/// On success: returns Result. dest is the on-disk file (caller does
/// SHA verify + atomic rename to final path via download::verify_and_promote).
///
/// On failure: returns Error. Caller maps to retry / bail by ErrorKind.
[[nodiscard]] std::expected<Result, Error> download_to_file(
    const std::string& url, const fs::path& dest, const Options& opts);

/// Fetch `url` body into a string. Used for small JSON / commit-sha
/// responses; not for large transfers (entire body buffered in memory).
[[nodiscard]] std::expected<std::string, Error> fetch_text(
    const std::string& url, int timeout_seconds);

/// Issue a HEAD request and return Content-Length. nullopt when absent
/// (chunked encoding) or the probe failed. Used by download.cpp to seed
/// progress::Bar with a real total so live frames can render percentage.
[[nodiscard]] std::optional<std::int64_t> head_content_length(
    const std::string& url, int timeout_seconds = 30);

}  // namespace luban::libcurl_backend
