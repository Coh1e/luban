#include "download.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
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

#include "log.hpp"

namespace luban::download {

namespace {

constexpr size_t kChunk = 1 << 16;        // 64 KiB
constexpr const wchar_t* kUserAgent = L"luban/0.1 (+https://github.com/luban)";

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
    session.h = WinHttpOpen(kUserAgent,
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

        std::string actual_sha = sha.finish();
        // 校验
        if (opts.expected_hash) {
            std::optional<hash::HashSpec> actual_other;
            const auto& exp = *opts.expected_hash;
            std::string actual_check_hex;
            if (exp.algo == hash::Algorithm::Sha256) {
                actual_check_hex = actual_sha;
            } else {
                // 其他算法：再读一遍文件做哈希
                actual_other = hash::hash_file(tmp, exp.algo);
                actual_check_hex = actual_other ? actual_other->hex : "";
            }
            if (actual_check_hex != exp.hex) {
                fs::remove(tmp, ec);
                return std::unexpected(Error{
                    ErrorKind::HashMismatch,
                    std::format("hash mismatch for {}\n  expected {}:{}\n  actual   {}:{}",
                                url, hash::algo_name(exp.algo), exp.hex,
                                hash::algo_name(exp.algo), actual_check_hex)
                });
            }
        }

        // atomic rename
        fs::remove(dest, ec);
        fs::rename(tmp, dest, ec);
        if (ec) {
            // 跨卷 fallback: copy + remove
            fs::copy_file(tmp, dest, fs::copy_options::overwrite_existing, ec);
            fs::remove(tmp, ec);
            if (ec) return std::unexpected(Error{ErrorKind::Io, "rename/copy failed"});
        }

        return DownloadResult{
            hash::HashSpec{hash::Algorithm::Sha256, actual_sha},
            *rc,
        };
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
