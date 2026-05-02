#include "download.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
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
#endif

#include "luban/version.hpp"

#include "log.hpp"

namespace luban::download {

namespace {

constexpr size_t kChunk = 1 << 16;        // 64 KiB

// User-Agent header. Built once from the cmake-injected version string so it
// always reflects the running binary (was hand-pinned at "0.1" pre-v0.2 and
// drifted out of sync with kVersion). WinHttpOpen wants UTF-16, so we widen.
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

#ifdef _WIN32

// ---- 进度条（stderr，TTY 时显示）----
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
        enabled = _isatty(_fileno(stderr)) && std::getenv("LUBAN_NO_PROGRESS") == nullptr;
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
        std::fprintf(stderr, "\r%s    ", line.c_str());
        std::fflush(stderr);
    }

    void finish() {
        if (!enabled) return;
        std::fprintf(stderr, "\r%-100s\r", "");
        std::fflush(stderr);
    }
};

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
// Returns the number of bytes actually written, or an error.
std::expected<int64_t, Error> download_range_to_file(
    const std::string& url, const fs::path& dest,
    int64_t lo, int64_t hi, int timeout_seconds)
{
    auto p = parse_url(url);
    if (!p) return std::unexpected(Error{ErrorKind::Network, "invalid URL: " + url});

    HttpHandle session;
    session.h = WinHttpOpen(user_agent().c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) return std::unexpected(Error{ErrorKind::Network, "WinHttpOpen failed"});
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

    log::stepf("chunked download: {} ({}, {} threads × ~{} MiB)",
               label, url, n, (per + 1024 * 1024 - 1) / (1024 * 1024));

    for (int i = 0; i < n; ++i) {
        threads.emplace_back([&, i] {
            results[i] = download_range_to_file(url, dest, ranges[i].lo, ranges[i].hi,
                                                timeout_seconds);
            if (results[i].has_value()) bytes_done.fetch_add(*results[i]);
        });
    }
    for (auto& t : threads) t.join();

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
        auto sha = hash::hash_file(tmp, hash::Algorithm::Sha256);
        if (!sha) {
            fs::remove(tmp, ec);
            return std::unexpected(Error{ErrorKind::Io, "post-download hash_file failed"});
        }
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
#endif  // _WIN32

}  // namespace

std::expected<DownloadResult, Error> download(
    const std::string& url, const fs::path& dest, const DownloadOptions& opts) {
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
        return DownloadResult{*promoted, *rc};
    }
    return std::unexpected(last_err);
#else
    (void)url; (void)dest; (void)opts;
    return std::unexpected(Error{ErrorKind::Network, "POSIX download not implemented"});
#endif
}

std::expected<std::string, Error> fetch_text(const std::string& url, int timeout_seconds) {
#ifdef _WIN32
    std::string buf;
    auto sink = [&](const unsigned char* data, size_t n, int64_t /*total*/)
        -> std::expected<void, Error> {
        buf.append(reinterpret_cast<const char*>(data), n);
        return {};
    };
    auto rc = do_request(url, timeout_seconds, sink);
    if (!rc.has_value()) return std::unexpected(rc.error());
    return buf;
#else
    (void)url; (void)timeout_seconds;
    return std::unexpected(Error{ErrorKind::Network, "POSIX not implemented"});
#endif
}

}  // namespace luban::download
