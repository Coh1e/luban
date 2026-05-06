#include "download.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <functional>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <io.h>
#include "util/win.hpp"
#else
#include <curl/curl.h>     // POSIX HTTP backend (ADR-0006 Phase B)
#include <unistd.h>        // mkstemp / close
#endif

#include "luban/version.hpp"

#include "log.hpp"

namespace luban::download {

namespace {

constexpr size_t kChunk = 1 << 16;        // 64 KiB

// Apply LUBAN_GITHUB_MIRROR_PREFIX to a github.com URL. The mirror format
// most CN/SEA reverse proxies use (ghfast.top, gh-proxy.com, etc.) is
// "<prefix>/<full-original-url>" — i.e., the original URL gets prepended
// to the mirror's host. Empty / unset env → no-op.
//
// We only rewrite a small allowlist of github-owned hosts; everything else
// (CDN-hosted assets, third-party download URLs, custom mirrors a blueprint
// already pointed at) passes through verbatim.
std::string apply_mirror(const std::string& url) {
    const char* env = std::getenv("LUBAN_GITHUB_MIRROR_PREFIX");
    if (!env || !*env) return url;
    // api.github.com is NOT proxied by ghfast.top / gh-proxy.com (they
    // 403 it). luban hits the API for release discovery during source
    // resolve, but the JSON is small enough that direct works even on
    // slow links; mirror only the bulk download paths.
    static const char* hosts[] = {
        "https://github.com/",
        "https://raw.githubusercontent.com/",
        "https://objects.githubusercontent.com/",
        "https://codeload.github.com/",
    };
    for (const char* h : hosts) {
        if (url.rfind(h, 0) == 0) {
            std::string out = env;
            while (!out.empty() && out.back() == '/') out.pop_back();
            return out + "/" + url;
        }
    }
    return url;
}

// ---- Progress bar (stderr, TTY only) ----
// Cross-platform: uses cstdio + chrono only. Both Win32 (do_request) and
// POSIX (libcurl-backed download) populate it via the sink callback.
struct Progress {
    std::string label;
    int64_t total = -1;       // -1 = 未知（Content-Length 缺失）
    int64_t done = 0;
    std::chrono::steady_clock::time_point t0;
    std::chrono::steady_clock::time_point last;
    bool enabled = false;

    Progress(std::string lbl, int64_t total_bytes) : label(std::move(lbl)), total(total_bytes) {
        t0 = std::chrono::steady_clock::now();
        last = t0;
        // TTY by default; LUBAN_PROGRESS=1 forces on (CI / wrapped shells
        // that swallow isatty); LUBAN_NO_PROGRESS=1 forces off.
        bool tty = _isatty(_fileno(stderr));
        bool force_on  = std::getenv("LUBAN_PROGRESS") != nullptr;
        bool force_off = std::getenv("LUBAN_NO_PROGRESS") != nullptr;
        enabled = (tty || force_on) && !force_off;
    }

    static std::string format_bytes(int64_t n) {
        const char* units[] = {"B", "KiB", "MiB", "GiB"};
        double f = static_cast<double>(n);
        int u = 0;
        while (f >= 1024.0 && u < 3) { f /= 1024.0; ++u; }
        if (u == 0) return std::format("{} B", n);
        return std::format("{:.1f} {}", f, units[u]);
    }

    void update(size_t n) {
        done += static_cast<int64_t>(n);
        if (!enabled) return;
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last).count();
        bool finished = (total > 0 && done >= total);
        if (dt < 0.1 && !finished) return;
        last = now;
        render(now);
    }

    void render(std::chrono::steady_clock::time_point now) {
        double elapsed = std::chrono::duration<double>(now - t0).count();
        if (elapsed < 1e-3) elapsed = 1e-3;
        double rate = static_cast<double>(done) / elapsed;
        std::string rate_str = format_bytes(static_cast<int64_t>(rate)) + "/s";
        std::string line;
        if (total > 0) {
            double pct = 100.0 * static_cast<double>(done) / static_cast<double>(total);
            constexpr int W = 24;
            int filled = static_cast<int>(W * static_cast<double>(done) / total);
            std::string bar;
            for (int i = 0; i < W; ++i) bar += (i < filled ? "#" : "\xc2\xb7");
            line = std::format("  [{}] {:5.1f}%  {}/{}  {}  {}",
                               bar, pct, format_bytes(done), format_bytes(total),
                               rate_str, label);
        } else {
            line = std::format("  {}  {}  {}", format_bytes(done), rate_str, label);
        }
        // Clear-line each frame so a shrinking line doesn't leave
        // tails (e.g. " ninja-w" stuck after the bar shifts to
        // "downloaded 9.7 MiB ..."). Same VT consideration as
        // finish(); legacy cmd.exe sees the literal escape but the
        // bar still lands.
        std::fprintf(stderr, "\r\x1b[2K%s", line.c_str());
        std::fflush(stderr);
    }

    void finish() {
        if (!enabled) return;
        // ANSI '\x1b[2K' clears the entire line regardless of width;
        // the fixed-width 100-space form left tails when the rendered
        // line was longer (chunked-mode label + multi-MB byte counts
        // routinely run >100 chars). Win10+ Conhost has VT processing
        // on for new shells; the few legacy cmd.exe windows that don't
        // will print the literal escape (cosmetic only, not breaking).
        std::fprintf(stderr, "\r\x1b[2K");
        std::fflush(stderr);
    }
};

// ---- Stall watchdog -------------------------------------------------------
// Real-world failure mode (VN -> github.com release CDN, 2026-05-06): a
// download starts at full speed, then the CDN gradually clamps the
// connection until throughput collapses to zero. Bytes still trickle in
// (a few hundred per minute), so WinHTTP's per-receive timeout never
// fires. The user sees "stuck at 14%" forever.
//
// StallDetector tracks bytes received over a rolling time window. When
// the average rate over the window drops below `kMinRate`, ok() returns
// false and the caller bails out. Outer retry loop then re-issues, which
// often succeeds because GitHub's clamp resets per-connection.
//
// Tuning: 15s warmup avoids false positives on the initial TLS handshake
// + DNS + first packet (which can collectively burn 3-5s on a high-RTT
// VPN). 1 KiB/s as the floor — any real download moves more than that.
struct StallDetector {
    using clock = std::chrono::steady_clock;
    clock::time_point window_start;
    int64_t window_bytes = 0;
    int64_t total_bytes = 0;
    static constexpr auto kWindow = std::chrono::seconds(15);
    static constexpr int64_t kMinRate = 1024;  // bytes / sec

    StallDetector() : window_start(clock::now()) {}

    void record(int64_t n) {
        window_bytes += n;
        total_bytes += n;
    }
    bool ok() {
        auto now = clock::now();
        auto dt = now - window_start;
        if (dt < kWindow) return true;  // warmup window
        auto secs = std::chrono::duration<double>(dt).count();
        if ((window_bytes / secs) < static_cast<double>(kMinRate)) return false;
        // Rate fine — slide window forward and continue.
        window_start = now;
        window_bytes = 0;
        return true;
    }
};

#ifdef _WIN32

// User-Agent header (wstring form). WinHttpOpen wants UTF-16. The POSIX
// libcurl path uses a parallel `user_agent_utf8()` returning std::string
// in the !_WIN32 block below.
const std::wstring& user_agent() {
    static const std::wstring s = [] {
        std::string narrow = "luban/" + std::string(luban::kLubanVersion)
                           + " (+https://github.com/Coh1e/luban)";
        std::wstring wide(narrow.size(), L' ');
        for (size_t i = 0; i < narrow.size(); ++i) wide[i] = static_cast<wchar_t>(narrow[i]);
        return wide;
    }();
    return s;
}

// ---- BCrypt 流式 SHA256 ----
struct StreamingSha256 {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    bool ok = false;

    StreamingSha256() {
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return;
        if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) != 0) {
            BCryptCloseAlgorithmProvider(alg, 0);
            alg = nullptr;
            return;
        }
        ok = true;
    }
    ~StreamingSha256() {
        if (h) BCryptDestroyHash(h);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    }
    void update(const unsigned char* data, ULONG n) {
        if (ok) BCryptHashData(h, const_cast<PUCHAR>(data), n, 0);
    }
    std::string finish() {
        if (!ok) return {};
        unsigned char digest[32];
        if (BCryptFinishHash(h, digest, sizeof(digest), 0) != 0) return {};
        static constexpr char hexc[] = "0123456789abcdef";
        std::string out(64, '\0');
        for (int i = 0; i < 32; ++i) {
            out[2*i]     = hexc[(digest[i] >> 4) & 0xF];
            out[2*i + 1] = hexc[digest[i] & 0xF];
        }
        return out;
    }
};

// ---- WinHTTP RAII ----
struct HttpHandle {
    HINTERNET h = nullptr;
    ~HttpHandle() { if (h) WinHttpCloseHandle(h); }
};

// 解析 url（只支持 https://host[:port]/path）—— manifest URL 都满足这个。
struct ParsedUrl {
    std::wstring host;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    std::wstring path;
    bool secure = true;
};

// Enable HTTP/2 on a WinHTTP session handle. HTTP/3 is opt-in via
// LUBAN_ENABLE_HTTP3=1 — see below for why.
//
// Empirical (VN -> github.com release CDN, 2026-05-06): single HTTP/1.1
// stream tops out around 50 KB/s; HTTP/2 over the same connection runs
// 4 MB/s — same throttle, but the protocol's multiplexing inside one
// TCP avoids the per-IP connection-count limit GitHub's CDN imposes.
//
// HTTP/3 (Win11 22H2+) was previously OR'd in unconditionally on the
// theory that "older Windows just ignores unknown flags". On Win11 with
// h3 actually negotiated, the QUIC handshake hangs ~10s before falling
// back to TCP/h2 on networks where UDP/443 is throttled or filtered
// (VN/CN networks routinely qualify). User reported `bp src update`
// taking 20s for a 10 KiB tarball — `curl --http2` against the same
// URL from the same machine: 780ms. 25× slowdown, root cause confirmed
// as h3 negotiation.
//
// Default HTTP/2 only. Opt-in to h3 with LUBAN_ENABLE_HTTP3=1 if you're
// on a network where UDP/443 to GitHub's CDN works.
//
// WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL is option 133, available since
// Windows 10 1607. Define constants ourselves so the build doesn't
// depend on which SDK header version is around.
#ifndef WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL
#define WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL 133
#endif
#ifndef WINHTTP_PROTOCOL_FLAG_HTTP2
#define WINHTTP_PROTOCOL_FLAG_HTTP2 0x1
#endif
#ifndef WINHTTP_PROTOCOL_FLAG_HTTP3
#define WINHTTP_PROTOCOL_FLAG_HTTP3 0x2
#endif
void enable_http2_on_session(HINTERNET session) {
    DWORD flags = WINHTTP_PROTOCOL_FLAG_HTTP2;
    if (std::getenv("LUBAN_ENABLE_HTTP3") != nullptr) {
        flags |= WINHTTP_PROTOCOL_FLAG_HTTP3;
    }
    WinHttpSetOption(session, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL,
                     &flags, sizeof(flags));
    // No error check: opt-in. If the OS doesn't recognize the option
    // (unusual — would require pre-1607), we degrade gracefully to
    // HTTP/1.1 instead of failing the request.
}

std::optional<ParsedUrl> parse_url(const std::string& url) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {0};
    wchar_t path[2048] = {0};
    uc.lpszHostName = host;
    uc.dwHostNameLength = static_cast<DWORD>(std::size(host));
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = static_cast<DWORD>(std::size(path));

    std::wstring wurl = win::from_utf8(url);
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return std::nullopt;
    if (uc.nScheme != INTERNET_SCHEME_HTTPS && uc.nScheme != INTERNET_SCHEME_HTTP)
        return std::nullopt;
    ParsedUrl out;
    out.host = std::wstring(host, uc.dwHostNameLength);
    out.path = std::wstring(path, uc.dwUrlPathLength);
    out.port = uc.nPort;
    out.secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    if (out.path.empty()) out.path = L"/";
    return out;
}

// 一次完整 HTTP 请求；按需 dump 到 sink（写文件 + 哈希 + 进度）或者 buffer（fetch_text）。
template <class Sink>
std::expected<int64_t, Error> do_request(
    const std::string& url, int timeout_seconds, Sink&& sink) {
    auto p = parse_url(url);
    if (!p) return std::unexpected(Error{ErrorKind::Network, "invalid URL: " + url});

    HttpHandle session;
    session.h = WinHttpOpen(user_agent().c_str(),
                            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) return std::unexpected(Error{ErrorKind::Network, "WinHttpOpen failed"});
    enable_http2_on_session(session.h);

    int ms = timeout_seconds * 1000;
    WinHttpSetTimeouts(session.h, ms, ms, ms, ms);

    HttpHandle conn;
    conn.h = WinHttpConnect(session.h, p->host.c_str(), p->port, 0);
    if (!conn.h) return std::unexpected(Error{ErrorKind::Network, "WinHttpConnect failed"});

    HttpHandle req;
    DWORD flags = p->secure ? WINHTTP_FLAG_SECURE : 0;
    req.h = WinHttpOpenRequest(conn.h, L"GET", p->path.c_str(), nullptr,
                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req.h) return std::unexpected(Error{ErrorKind::Network, "WinHttpOpenRequest failed"});

    // 关闭证书检查的"放任"开关——默认 winhttp 会验证 cert chain（系统 CA store），
    // 这就是我们想要的安全默认。

    // 跟随重定向（默认行为，但显式声明）。
    DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(req.h, WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof(redir));

    if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        return std::unexpected(Error{ErrorKind::Network, "WinHttpSendRequest failed"});
    }
    if (!WinHttpReceiveResponse(req.h, nullptr)) {
        return std::unexpected(Error{ErrorKind::Network, "WinHttpReceiveResponse failed"});
    }

    DWORD status = 0;
    DWORD slen = sizeof(status);
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen,
                        WINHTTP_NO_HEADER_INDEX);
    if (status >= 400) {
        ErrorKind k = (status < 500) ? ErrorKind::HttpClient : ErrorKind::HttpServer;
        return std::unexpected(Error{k, std::format("HTTP {} for {}", status, url)});
    }

    int64_t total = -1;
    {
        wchar_t cl[64] = {0};
        DWORD cllen = sizeof(cl);
        if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_LENGTH,
                                WINHTTP_HEADER_NAME_BY_INDEX, cl, &cllen,
                                WINHTTP_NO_HEADER_INDEX)) {
            try { total = std::stoll(win::to_utf8(cl)); } catch (...) { total = -1; }
        }
    }

    int64_t done = 0;
    std::vector<unsigned char> buf(kChunk);
    StallDetector stall;
    while (true) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.h, &avail))
            return std::unexpected(Error{ErrorKind::Network, "WinHttpQueryDataAvailable failed"});
        if (avail == 0) break;
        if (avail > buf.size()) buf.resize(avail);
        DWORD got = 0;
        if (!WinHttpReadData(req.h, buf.data(), avail, &got))
            return std::unexpected(Error{ErrorKind::Network, "WinHttpReadData failed"});
        if (got == 0) break;
        if (auto e = sink(buf.data(), got, total); !e.has_value()) return std::unexpected(e.error());
        done += got;
        stall.record(got);
        if (!stall.ok()) {
            return std::unexpected(Error{ErrorKind::Network,
                "transfer stalled (<1 KiB/s for 15s)"});
        }
    }
    return done;
}

// ---- HEAD probe + chunked Range download (Phase 14b) -----------------------
//
// Decision tree (in download() below):
//   parallel_chunks <= 1            → single-stream (do_request)
//   HEAD fails OR no Content-Length → single-stream (caller doesn't know size)
//   no Accept-Ranges: bytes header  → single-stream (server can't slice)
//   Content-Length < threshold      → single-stream (overhead > benefit)
//   else                            → download_chunked
//
// Chunked path opens one WinHttp session per worker thread (handles aren't
// thread-safe), each fetches `Range: bytes=<lo>-<hi>` and writes to its slice
// of a pre-allocated output file. Hash verification runs once at the end via
// hash::verify_file (re-reads the file) since concurrent writers can't share
// a streaming-hash state.

struct HeadInfo {
    int64_t content_length = -1;       // -1 if header missing / unparseable
    bool accepts_ranges = false;       // true iff `Accept-Ranges: bytes`
};

// HEAD probe. Same WinHttp shape as do_request but with method=HEAD and we
// only inspect headers (no body to read). Failures yield `nullopt` so the
// caller can gracefully fall back to single-stream.
std::optional<HeadInfo> head_info(const std::string& url, int timeout_seconds) {
    auto p = parse_url(url);
    if (!p) return std::nullopt;

    HttpHandle session;
    session.h = WinHttpOpen(user_agent().c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) return std::nullopt;
    enable_http2_on_session(session.h);
    int ms = timeout_seconds * 1000;
    WinHttpSetTimeouts(session.h, ms, ms, ms, ms);

    HttpHandle conn;
    conn.h = WinHttpConnect(session.h, p->host.c_str(), p->port, 0);
    if (!conn.h) return std::nullopt;

    HttpHandle req;
    DWORD flags = p->secure ? WINHTTP_FLAG_SECURE : 0;
    req.h = WinHttpOpenRequest(conn.h, L"HEAD", p->path.c_str(), nullptr,
                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req.h) return std::nullopt;

    DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(req.h, WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof(redir));

    if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
        return std::nullopt;
    if (!WinHttpReceiveResponse(req.h, nullptr)) return std::nullopt;

    DWORD status = 0;
    DWORD slen = sizeof(status);
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen,
                        WINHTTP_NO_HEADER_INDEX);
    if (status >= 400) return std::nullopt;

    HeadInfo h;
    {
        wchar_t cl[64] = {0};
        DWORD cllen = sizeof(cl);
        if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_LENGTH,
                                WINHTTP_HEADER_NAME_BY_INDEX, cl, &cllen,
                                WINHTTP_NO_HEADER_INDEX)) {
            try { h.content_length = std::stoll(win::to_utf8(cl)); }
            catch (...) { h.content_length = -1; }
        }
    }
    {
        wchar_t ar[64] = {0};
        DWORD arlen = sizeof(ar);
        // WINHTTP_QUERY_ACCEPT_RANGES = 0x0027 — well-known header index.
        if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_ACCEPT_RANGES,
                                WINHTTP_HEADER_NAME_BY_INDEX, ar, &arlen,
                                WINHTTP_NO_HEADER_INDEX)) {
            std::string v = win::to_utf8(std::wstring(ar, arlen / sizeof(wchar_t)));
            // "bytes" or "bytes,foo" → supports byte ranges. "none" → no.
            for (auto& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (v.find("bytes") != std::string::npos) h.accepts_ranges = true;
        }
    }
    return h;
}

// Fetch one byte range [lo, hi] (inclusive on both ends per RFC 7233) and
// write it directly into `dest` at offset `lo`. Each call creates its own
// WinHttp + file handles so it's safe to run from multiple threads.
//
// `on_bytes`, when non-empty, is called after each successful WriteFile
// with the number of bytes just written. Used by download_chunked to
// drive a shared Progress bar across N worker threads.
//
// Returns the number of bytes actually written, or an error.
std::expected<int64_t, Error> download_range_to_file(
    const std::string& url, const fs::path& dest,
    int64_t lo, int64_t hi, int timeout_seconds,
    const std::function<void(size_t)>& on_bytes)
{
    auto p = parse_url(url);
    if (!p) return std::unexpected(Error{ErrorKind::Network, "invalid URL: " + url});

    HttpHandle session;
    session.h = WinHttpOpen(user_agent().c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) return std::unexpected(Error{ErrorKind::Network, "WinHttpOpen failed"});
    enable_http2_on_session(session.h);
    int ms = timeout_seconds * 1000;
    WinHttpSetTimeouts(session.h, ms, ms, ms, ms);

    HttpHandle conn;
    conn.h = WinHttpConnect(session.h, p->host.c_str(), p->port, 0);
    if (!conn.h) return std::unexpected(Error{ErrorKind::Network, "WinHttpConnect failed"});

    HttpHandle req;
    DWORD flags = p->secure ? WINHTTP_FLAG_SECURE : 0;
    req.h = WinHttpOpenRequest(conn.h, L"GET", p->path.c_str(), nullptr,
                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req.h) return std::unexpected(Error{ErrorKind::Network, "WinHttpOpenRequest failed"});

    DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(req.h, WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof(redir));

    // Range header. RFC 7233 inclusive on both sides.
    std::wstring range_hdr = L"Range: bytes=" +
        std::to_wstring(lo) + L"-" + std::to_wstring(hi);
    if (!WinHttpAddRequestHeaders(req.h, range_hdr.c_str(),
                                  static_cast<DWORD>(range_hdr.size()),
                                  WINHTTP_ADDREQ_FLAG_ADD)) {
        return std::unexpected(Error{ErrorKind::Network, "WinHttpAddRequestHeaders Range failed"});
    }

    if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
        return std::unexpected(Error{ErrorKind::Network, "WinHttpSendRequest failed"});
    if (!WinHttpReceiveResponse(req.h, nullptr))
        return std::unexpected(Error{ErrorKind::Network, "WinHttpReceiveResponse failed"});

    DWORD status = 0;
    DWORD slen = sizeof(status);
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen,
                        WINHTTP_NO_HEADER_INDEX);
    // Expected: 206 Partial Content. Some servers return 200 if the range
    // covers the whole file — accept that too.
    if (status != 206 && status != 200) {
        ErrorKind k = (status < 500) ? ErrorKind::HttpClient : ErrorKind::HttpServer;
        return std::unexpected(Error{k,
            std::format("range request returned HTTP {} (expected 206)", status)});
    }

    // Open per-thread file handle. FILE_SHARE_WRITE lets sibling threads
    // also have their own handle open at disjoint offsets; NTFS handles the
    // disjoint write parallelism correctly without further synchronization.
    HANDLE fh = CreateFileW(dest.wstring().c_str(),
                            GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr,
                            OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL,
                            nullptr);
    if (fh == INVALID_HANDLE_VALUE) {
        return std::unexpected(Error{ErrorKind::Io, "CreateFile (chunk dest) failed"});
    }

    LARGE_INTEGER off; off.QuadPart = lo;
    if (!SetFilePointerEx(fh, off, nullptr, FILE_BEGIN)) {
        CloseHandle(fh);
        return std::unexpected(Error{ErrorKind::Io, "SetFilePointerEx failed"});
    }

    int64_t written = 0;
    std::vector<unsigned char> buf(kChunk);
    StallDetector stall;
    while (true) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.h, &avail)) {
            CloseHandle(fh);
            return std::unexpected(Error{ErrorKind::Network, "WinHttpQueryDataAvailable failed"});
        }
        if (avail == 0) break;
        if (avail > buf.size()) buf.resize(avail);
        DWORD got = 0;
        if (!WinHttpReadData(req.h, buf.data(), avail, &got)) {
            CloseHandle(fh);
            return std::unexpected(Error{ErrorKind::Network, "WinHttpReadData failed"});
        }
        if (got == 0) break;
        DWORD wrote = 0;
        if (!WriteFile(fh, buf.data(), got, &wrote, nullptr) || wrote != got) {
            CloseHandle(fh);
            return std::unexpected(Error{ErrorKind::Io, "WriteFile (chunk) failed"});
        }
        written += wrote;
        if (on_bytes) on_bytes(static_cast<size_t>(wrote));
        stall.record(wrote);
        if (!stall.ok()) {
            CloseHandle(fh);
            return std::unexpected(Error{ErrorKind::Network,
                "transfer stalled (<1 KiB/s for 15s)"});
        }
    }
    CloseHandle(fh);
    return written;
}

// Pre-allocate `dest` to `total` bytes so each thread can SetFilePointerEx +
// WriteFile into its slice without extending the file. SetEndOfFile sets
// physical size; allocation is sparse on NTFS (no zero-fill cost).
bool preallocate_file(const fs::path& dest, int64_t total) {
    HANDLE fh = CreateFileW(dest.wstring().c_str(),
                            GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr,
                            CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL,
                            nullptr);
    if (fh == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER eof; eof.QuadPart = total;
    bool ok = SetFilePointerEx(fh, eof, nullptr, FILE_BEGIN) && SetEndOfFile(fh);
    CloseHandle(fh);
    return ok;
}

// Orchestrate N parallel range fetches into `dest`. Caller has already
// confirmed the server supports byte ranges and `total` is the
// Content-Length. Returns total bytes written or first error.
std::expected<int64_t, Error> download_chunked(
    const std::string& url, const fs::path& dest,
    int64_t total, int n, int timeout_seconds, const std::string& label)
{
    if (n < 2) n = 2;
    if (n > 16) n = 16;  // sanity cap; CDNs throttle anyway after 4-8 conns

    if (!preallocate_file(dest, total)) {
        return std::unexpected(Error{ErrorKind::Io, "could not preallocate output file"});
    }

    // Compute disjoint inclusive ranges. Last chunk takes any remainder.
    struct Range { int64_t lo, hi; };
    std::vector<Range> ranges;
    int64_t per = total / n;
    for (int i = 0; i < n; ++i) {
        int64_t lo = i * per;
        int64_t hi = (i == n - 1) ? (total - 1) : (lo + per - 1);
        ranges.push_back({lo, hi});
    }

    std::vector<std::expected<int64_t, Error>> results(n);
    std::vector<std::thread> threads;
    threads.reserve(n);
    std::atomic<int64_t> bytes_done{0};

    log::infof("  chunked download: {} threads × ~{} MiB",
               n, (per + 1024 * 1024 - 1) / (1024 * 1024));

    // Shared live progress: each worker thread reports bytes written via
    // on_bytes; a mutex serializes Progress::update calls (which throttles
    // its own re-render to 0.1 s, so contention is bounded).
    Progress prog(label, total);
    std::mutex prog_m;
    auto on_bytes = [&](size_t k) {
        std::lock_guard<std::mutex> g(prog_m);
        prog.update(k);
    };

    for (int i = 0; i < n; ++i) {
        threads.emplace_back([&, i] {
            results[i] = download_range_to_file(url, dest, ranges[i].lo, ranges[i].hi,
                                                timeout_seconds, on_bytes);
            if (results[i].has_value()) bytes_done.fetch_add(*results[i]);
        });
    }
    for (auto& t : threads) t.join();
    prog.finish();

    int64_t total_written = 0;
    for (int i = 0; i < n; ++i) {
        if (!results[i].has_value()) return std::unexpected(results[i].error());
        total_written += *results[i];
    }
    if (total_written != total) {
        return std::unexpected(Error{ErrorKind::Network,
            std::format("chunked total mismatch: wrote {} bytes, expected {}",
                        total_written, total)});
    }
    return total_written;
}

#endif  // _WIN32 (closes the Win32-specific helpers block — verify_and_promote
        // and the POSIX block below are cross-platform / POSIX-only)

// Common post-download finalization: verify hash + atomic rename to dest.
// Both the chunked and single-stream branches need exactly this sequence
// after writing tmp; extracting it into a helper removes ~30 lines of
// near-duplicate code from each branch (F6).
//
// `precomputed_sha256_hex` lets the single-stream path pass its
// streaming-computed sha256 to skip a re-read; the chunked path leaves it
// nullopt because concurrent writers can't share a streaming hash state,
// so it always recomputes via hash::hash_file. If `expected_hash` is set
// to a non-sha256 algo, we re-read regardless.
//
// On any failure, tmp is removed and the original Error is returned.
// On success, tmp has been moved to dest and its sha256 HashSpec is returned.
//
// Cross-platform: uses hash::hash_file (POSIX impl via OpenSSL EVP) +
// std::filesystem only.
std::expected<hash::HashSpec, Error> verify_and_promote(
    const fs::path& tmp,
    const fs::path& dest,
    const std::optional<hash::HashSpec>& expected,
    const std::string& url,
    const std::optional<std::string>& precomputed_sha256_hex = std::nullopt) {
    std::error_code ec;

    // Compute (or accept) the sha256 of the tmp file.
    std::string sha256_hex;
    if (precomputed_sha256_hex) {
        sha256_hex = *precomputed_sha256_hex;
    } else {
        // Re-read pass — only the chunked path lands here. Show a status
        // line because for ~200 MB artifacts this is 1-2 s of dead air
        // after download finishes.
        std::error_code sz_ec;
        auto sz = fs::file_size(tmp, sz_ec);
        log::infof("  verifying SHA256 ({} MiB)...", sz_ec ? 0 : sz / (1024 * 1024));
        auto t0 = std::chrono::steady_clock::now();
        auto sha = hash::hash_file(tmp, hash::Algorithm::Sha256);
        auto dt = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count();
        if (!sha) {
            fs::remove(tmp, ec);
            return std::unexpected(Error{ErrorKind::Io, "post-download hash_file failed"});
        }
        log::infof("  SHA256 verified in {:.1f}s", dt);
        sha256_hex = sha->hex;
    }

    // Hash check against expected (if caller specified one).
    if (expected) {
        const auto& exp = *expected;
        std::string actual_hex;
        if (exp.algo == hash::Algorithm::Sha256) {
            actual_hex = sha256_hex;
        } else {
            // Non-sha256 expected algo — re-read the file with that algo.
            auto h = hash::hash_file(tmp, exp.algo);
            actual_hex = h ? h->hex : "";
        }
        if (actual_hex != exp.hex) {
            fs::remove(tmp, ec);
            return std::unexpected(Error{
                ErrorKind::HashMismatch,
                std::format("hash mismatch for {}\n  expected {}:{}\n  actual   {}:{}",
                            url, hash::algo_name(exp.algo), exp.hex,
                            hash::algo_name(exp.algo), actual_hex)});
        }
    }

    // Atomic rename tmp → dest with cross-volume copy fallback.
    fs::remove(dest, ec);
    fs::rename(tmp, dest, ec);
    if (ec) {
        ec.clear();
        fs::copy_file(tmp, dest, fs::copy_options::overwrite_existing, ec);
        std::error_code rm_ec;
        fs::remove(tmp, rm_ec);
        if (ec) return std::unexpected(Error{ErrorKind::Io, "rename/copy failed"});
    }
    return hash::HashSpec{hash::Algorithm::Sha256, sha256_hex};
}

#ifndef _WIN32
// ---- POSIX HTTP backend (libcurl) ----
//
// Minimum-viable Phase B half 2: single-stream download via curl_easy.
// No chunked Range support yet (would need head_info + Range request +
// concurrent CURL handles); deferred to a follow-up PR. The retry +
// hash-verify + atomic-rename machinery (download() / verify_and_promote)
// is shared with the Win32 path now that verify_and_promote moved out
// of the #ifdef _WIN32 block.

const std::string& user_agent_utf8() {
    // Mirrors the wstring user_agent() on Win32 — same UA string just in
    // UTF-8 since libcurl's CURLOPT_USERAGENT wants `const char*`.
    static const std::string ua =
        std::string("luban/") + luban::kLubanVersion + " (+https://github.com/Coh1e/luban)";
    return ua;
}

// libcurl write callback. userdata points to a Sink&-typed lambda
// stored in CurlSinkCtx. Returning fewer bytes than supplied aborts
// the transfer (libcurl maps that to CURLE_WRITE_ERROR).
template <class Sink>
struct CurlSinkCtx {
    Sink* sink;
    int64_t total = -1;
    std::optional<Error> err;
};

template <class Sink>
size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* c = static_cast<CurlSinkCtx<Sink>*>(userdata);
    size_t n = size * nmemb;
    auto rc = (*c->sink)(reinterpret_cast<const unsigned char*>(ptr), n, c->total);
    if (!rc) { c->err = rc.error(); return 0; }
    return n;
}

template <class Sink>
std::expected<int64_t, Error> do_request(
    const std::string& url, int timeout_seconds, Sink&& sink) {
    CURL* curl = curl_easy_init();
    if (!curl) return std::unexpected(Error{ErrorKind::Network, "curl_easy_init failed"});

    CurlSinkCtx<Sink> ctx{&sink, -1, std::nullopt};

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent_utf8().c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout_seconds));
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 0L);   // we inspect status ourselves
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb<Sink>);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

    // Probe Content-Length up front via HEAD-equivalent isn't done here —
    // libcurl gives us CURLINFO_CONTENT_LENGTH_DOWNLOAD_T after perform.
    // For Progress display, the sink can read total from the ctx if we
    // populate it in a header-callback; keeping minimal for now.

    CURLcode rc = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_off_t bytes_dl = 0;
    curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD_T, &bytes_dl);

    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        // Sink-injected error wins (more specific than libcurl's generic
        // CURLE_WRITE_ERROR when the sink itself rejected a chunk).
        if (ctx.err) return std::unexpected(*ctx.err);
        return std::unexpected(Error{
            ErrorKind::Network,
            std::format("libcurl: {} (curl error {})",
                        curl_easy_strerror(rc), static_cast<int>(rc))});
    }
    if (http_code >= 400 && http_code < 500) {
        return std::unexpected(Error{
            ErrorKind::HttpClient,
            std::format("HTTP {} for {}", http_code, url)});
    }
    if (http_code >= 500) {
        return std::unexpected(Error{
            ErrorKind::Network,
            std::format("HTTP {} for {}", http_code, url)});
    }
    return static_cast<int64_t>(bytes_dl);
}
#endif  // !_WIN32

}  // namespace

std::expected<DownloadResult, Error> download(
    const std::string& url_in, const fs::path& dest, const DownloadOptions& opts) {
    const std::string url = apply_mirror(url_in);
    auto t_start = std::chrono::steady_clock::now();
    auto summary = [&](int64_t bytes) {
        double dt = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t_start).count();
        if (dt < 1e-3) dt = 1e-3;
        double rate = static_cast<double>(bytes) / dt;
        // log::ok prepends its own green ✓; don't duplicate it in the
        // body. Indent with two spaces to slot under the "[i/N] tool"
        // step header from blueprint_apply.
        log::okf("  downloaded {} in {:.1f}s ({}/s)",
                 Progress::format_bytes(bytes), dt,
                 Progress::format_bytes(static_cast<int64_t>(rate)));
    };
#ifdef _WIN32
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);

    std::string label = opts.label.empty()
        ? (url.rfind('/') != std::string::npos ? url.substr(url.rfind('/') + 1) : url)
        : opts.label;

    // Chunked-download decision. We do this BEFORE the retry loop because a
    // failed HEAD or unsupported Range never recovers on retry — we just
    // fall back to single-stream and start the loop with that strategy.
    bool use_chunked = false;
    int64_t known_size = -1;
    if (opts.parallel_chunks > 1) {
        if (auto h = head_info(url, opts.timeout_seconds)) {
            if (h->accepts_ranges
                && h->content_length >= opts.chunk_threshold) {
                use_chunked = true;
                known_size = h->content_length;
            }
        }
        // Fall-through: single-stream if HEAD failed or server doesn't
        // chunk. We don't log a warning here because for many small files
        // the HEAD probe is the expected outcome of "not chunked".
    }

    if (use_chunked) {
        // Chunked path: pre-allocate dest, N threads do Range GETs, single
        // verify_file at end (concurrent writers can't share a streaming
        // hash state). Retries: chunked failure falls back to single-stream
        // (we don't retry chunked itself — a partial output file from a
        // half-completed chunked download is hard to resume cleanly).
        wchar_t tmp_buf[MAX_PATH * 2];
        std::wstring parent = dest.parent_path().wstring();
        std::wstring prefix = L".dl-";
        if (GetTempFileNameW(parent.c_str(), prefix.c_str(), 0, tmp_buf) == 0) {
            return std::unexpected(Error{ErrorKind::Io, "GetTempFileName failed"});
        }
        fs::path tmp = fs::path(tmp_buf);
        // GetTempFileName creates a 0-byte file; preallocate_file then
        // CREATE_ALWAYS-overwrites it to the right size.

        auto rc = download_chunked(url, tmp, known_size,
                                   opts.parallel_chunks, opts.timeout_seconds, label);
        if (!rc.has_value()) {
            log::warnf("chunked download failed ({}); falling back to single-stream",
                       rc.error().message);
            fs::remove(tmp, ec);
            // Fall through to single-stream below.
        } else {
            // Chunked path can't share a streaming hash state across N
            // threads — verify_and_promote re-reads the assembled file.
            // Slight cost (~0.5s per 100 MB on SSD), only correct option.
            auto promoted = verify_and_promote(tmp, dest, opts.expected_hash, url);
            if (!promoted) return std::unexpected(promoted.error());
            summary(*rc);
            return DownloadResult{*promoted, *rc};
        }
    }

    Error last_err{ErrorKind::Network, "no attempts"};
    int retries = std::max(1, opts.retries);
    for (int attempt = 1; attempt <= retries; ++attempt) {
        // tmp 文件：dest 同目录下 .dl-<rand>，避免跨卷 rename。
        wchar_t tmp_buf[MAX_PATH * 2];
        std::wstring parent = dest.parent_path().wstring();
        std::wstring prefix = L".dl-";
        if (GetTempFileNameW(parent.c_str(), prefix.c_str(), 0, tmp_buf) == 0) {
            return std::unexpected(Error{ErrorKind::Io, "GetTempFileName failed"});
        }
        fs::path tmp = fs::path(tmp_buf);

        FILE* fp = nullptr;
#ifdef _MSC_VER
        if (_wfopen_s(&fp, tmp.wstring().c_str(), L"wb") != 0) fp = nullptr;
#else
        fp = _wfopen(tmp.wstring().c_str(), L"wb");
#endif
        if (!fp) {
            fs::remove(tmp, ec);
            return std::unexpected(Error{ErrorKind::Io, "open tmp failed"});
        }

        StreamingSha256 sha;
        std::optional<Progress> prog;

        auto sink = [&](const unsigned char* data, size_t n, int64_t total)
            -> std::expected<void, Error> {
            if (!prog) prog.emplace(label, total);
            if (std::fwrite(data, 1, n, fp) != n) {
                return std::unexpected(Error{ErrorKind::Io, "fwrite failed"});
            }
            sha.update(data, static_cast<ULONG>(n));
            prog->update(n);
            return {};
        };

        auto rc = do_request(url, opts.timeout_seconds, sink);
        if (prog) prog->finish();
        std::fclose(fp);

        if (!rc.has_value()) {
            last_err = rc.error();
            fs::remove(tmp, ec);
            // 4xx 不重试
            if (last_err.kind == ErrorKind::HttpClient) return std::unexpected(last_err);
            // 退避后重试
            if (attempt < retries) {
                int wait = 1 << (attempt - 1);
                log::warnf("download attempt {} failed ({}); retry in {}s", attempt, last_err.message, wait);
                std::this_thread::sleep_for(std::chrono::seconds(wait));
            }
            continue;
        }

        // Single-stream path computed sha256 incrementally — pass to
        // verify_and_promote so it can skip the re-read for the sha256
        // check (the non-sha256 case still re-reads, by design).
        std::string actual_sha = sha.finish();
        auto promoted = verify_and_promote(
            tmp, dest, opts.expected_hash, url, actual_sha);
        if (!promoted) return std::unexpected(promoted.error());
        summary(*rc);
        return DownloadResult{*promoted, *rc};
    }
    return std::unexpected(last_err);
#else
    // POSIX download path (libcurl). Minimum-viable Phase B half 2:
    // single-stream only (no chunked Range yet). Hash is recomputed on
    // disk via verify_and_promote — slightly slower than Win32's
    // streaming sha but correct and unblocks `luban setup` end-to-end.
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);

    std::string label = opts.label.empty()
        ? (url.rfind('/') != std::string::npos ? url.substr(url.rfind('/') + 1) : url)
        : opts.label;

    Error last_err{ErrorKind::Network, "no attempts"};
    int retries = std::max(1, opts.retries);
    for (int attempt = 1; attempt <= retries; ++attempt) {
        // mkstemp creates an o600 file in dest's parent dir, returning fd.
        // The XXXXXX template gets replaced in-place; we pull the resulting
        // path out via the buffer.
        std::string tmpl = (dest.parent_path() / ".dl-XXXXXX").string();
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        int fd = mkstemp(buf.data());
        if (fd < 0) return std::unexpected(Error{ErrorKind::Io, "mkstemp failed"});
        fs::path tmp = fs::path(buf.data());

        FILE* fp = fdopen(fd, "wb");
        if (!fp) {
            ::close(fd);
            fs::remove(tmp, ec);
            return std::unexpected(Error{ErrorKind::Io, "fdopen failed"});
        }

        std::optional<Progress> prog;
        auto sink = [&](const unsigned char* data, size_t n, int64_t total)
            -> std::expected<void, Error> {
            if (!prog) prog.emplace(label, total);
            if (std::fwrite(data, 1, n, fp) != n) {
                return std::unexpected(Error{ErrorKind::Io, "fwrite failed"});
            }
            prog->update(n);
            return {};
        };

        auto rc = do_request(url, opts.timeout_seconds, sink);
        if (prog) prog->finish();
        std::fclose(fp);

        if (!rc.has_value()) {
            last_err = rc.error();
            fs::remove(tmp, ec);
            // 4xx not retried — server says it'll never succeed.
            if (last_err.kind == ErrorKind::HttpClient) return std::unexpected(last_err);
            if (attempt < retries) {
                int wait = 1 << (attempt - 1);
                log::warnf("download attempt {} failed ({}); retry in {}s",
                           attempt, last_err.message, wait);
                std::this_thread::sleep_for(std::chrono::seconds(wait));
            }
            continue;
        }

        // No streaming sha on POSIX yet — verify_and_promote recomputes
        // by re-reading. Future: parallel POSIX StreamingSha256 via EVP.
        auto promoted = verify_and_promote(tmp, dest, opts.expected_hash, url);
        if (!promoted) return std::unexpected(promoted.error());
        summary(*rc);
        return DownloadResult{*promoted, *rc};
    }
    return std::unexpected(last_err);
#endif
}

std::expected<std::string, Error> fetch_text(const std::string& url_in, int timeout_seconds) {
    const std::string url = apply_mirror(url_in);
    // Cross-platform now — do_request has both Win32 and POSIX impls.
    std::string buf;
    auto sink = [&](const unsigned char* data, size_t n, int64_t /*total*/)
        -> std::expected<void, Error> {
        buf.append(reinterpret_cast<const char*>(data), n);
        return {};
    };
    auto rc = do_request(url, timeout_seconds, sink);
    if (!rc.has_value()) return std::unexpected(rc.error());
    return buf;
}

}  // namespace luban::download
