#include "download.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <functional>
#include <mutex>
#include <optional>
#include <random>
#include <thread>
#include <vector>

#ifdef _WIN32
// Win32 download backend lives in src/curl_subprocess.{hpp,cpp} — drives
// curl.exe (Win10 1803+ baseline) instead of WinHTTP. See that file's
// header for the saga; this file's Win32 branches are now thin adapters.
// We still pull in <windows.h> for GetTempFileNameW + MAX_PATH used in
// download()'s Win32 branch to manufacture a tmp file path.
#include <windows.h>
#include "curl_subprocess.hpp"
#else
#include <curl/curl.h>     // POSIX HTTP backend (ADR-0006 Phase B)
#include <unistd.h>        // mkstemp / close
#endif

#include "luban/version.hpp"

#include "log.hpp"
#include "progress.hpp"

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
// Live progress for downloads now lives in src/progress.{hpp,cpp} as
// luban::progress::Bar — same class is reused by archive::extract via
// store_fetch.cpp, giving a unified UI language across phases. See
// progress.hpp for the format spec ("↓ fetch [▓▓▓░░░] 73% 2.9/4.0 MiB
// @ 3.0 MiB/s" live, "✓ fetch 4.0 MiB in 1.3s @ 3.0 MiB/s" on done).

// ---- Stall watchdog -------------------------------------------------------
// On Win32, stall detection lives inside curl_subprocess::run_curl (file-
// size-delta polling thread terminates curl if no growth for 15s after
// warmup). On POSIX it's still inline below in the libcurl path.

// Win32 backend used to live here as ~520 lines of WinHTTP code (StreamingSha256,
// HttpHandle RAII, parse_url, do_request, head_info, download_range_to_file,
// preallocate_file, download_chunked, enable_http2_on_session). All replaced
// by curl_subprocess. The `download()` Win32 branch below dispatches to it
// directly. POSIX libcurl path further down stays in-process. See git
// history for the deleted code.

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
    // Progress::Bar's finish_done() prints the unified "✓ fetch X in Ys
    // @ rate" summary line itself, so we no longer compose one here.
    // The bar's elapsed/rate counters are accurate end-to-end (start
    // before any I/O, finish on the last successful byte).
#ifdef _WIN32
    // v0.3.0 — Win32 download path is now a thin adapter over curl.exe.
    // No HEAD pre-probe for chunked split (single curl stream is fine on
    // GitHub's CDN); no streaming SHA (verify_and_promote re-reads, ~200ms
    // per 50MB on SSD); no parallel_chunks branch (single stream
    // intentionally — multi-stream tripped the per-IP throttle).
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);

    std::string label = opts.label.empty()
        ? (url.rfind('/') != std::string::npos ? url.substr(url.rfind('/') + 1) : url)
        : opts.label;

    // tmp file in dest's parent dir so cross-volume rename never bites.
    // GetTempFileName creates a 0-byte placeholder; curl -C - writes to it
    // and resumes from existing size on retry.
    wchar_t tmp_buf[MAX_PATH * 2];
    std::wstring parent = dest.parent_path().wstring();
    std::wstring prefix = L".dl-";
    if (GetTempFileNameW(parent.c_str(), prefix.c_str(), 0, tmp_buf) == 0) {
        return std::unexpected(Error{ErrorKind::Io, "GetTempFileName failed"});
    }
    fs::path tmp = fs::path(tmp_buf);
    // GetTempFileName already wrote a 0-byte file; -C - on first try reads
    // 0, behaves like normal GET. Across retries the same tmp persists with
    // partial bytes for resume.

    // HEAD probe for Content-Length so the bar shows pct from the first
    // frame. Optional — nullopt → bar shows running count + rate.
    std::int64_t total = -1;
    if (auto cl = luban::curl_subprocess::head_content_length(url,
                                                              opts.timeout_seconds)) {
        total = *cl;
    }

    luban::progress::Bar prog(luban::progress::Action::fetch(), total,
                              luban::progress::Unit::Bytes, label);

    Error last_err{ErrorKind::Network, "no attempts"};
    int retries = std::max(1, opts.retries);
    for (int attempt = 1; attempt <= retries; ++attempt) {
        luban::curl_subprocess::Options copts;
        copts.connect_timeout_seconds = std::min(opts.timeout_seconds, 30);
        copts.max_time_seconds = std::max(900, opts.timeout_seconds * 2);
        copts.resume = true;
        copts.progress = &prog;

        auto rc = luban::curl_subprocess::download_to_file(url, tmp, copts);
        if (rc) {
            prog.finish_done();
            // verify_and_promote re-reads tmp for SHA256 (no streaming hash
            // on the subprocess path) — accepted cost, ~200ms / 50MB.
            auto promoted = verify_and_promote(tmp, dest, opts.expected_hash, url);
            if (!promoted) return std::unexpected(promoted.error());
            return DownloadResult{*promoted, rc->bytes};
        }

        // Translate curl_subprocess::ErrorKind → download::ErrorKind for
        // the public API. retry decision lives here.
        const auto& e = rc.error();
        ErrorKind k = ErrorKind::Network;
        switch (e.kind) {
            case luban::curl_subprocess::ErrorKind::HttpClient:
                k = ErrorKind::HttpClient; break;
            case luban::curl_subprocess::ErrorKind::HttpServer:
                k = ErrorKind::HttpServer; break;
            case luban::curl_subprocess::ErrorKind::Stalled:
            case luban::curl_subprocess::ErrorKind::Network:
            case luban::curl_subprocess::ErrorKind::SpawnFailed:
            case luban::curl_subprocess::ErrorKind::NotFound:
                k = ErrorKind::Network; break;
            case luban::curl_subprocess::ErrorKind::Io:
                k = ErrorKind::Io; break;
        }
        last_err = Error{k, e.message};
        if (k == ErrorKind::HttpClient) {
            // 4xx — bail without retry. partial tmp already cleaned up
            // by curl_subprocess. Don't print a misleading ✓ via finish_done.
            prog.abandon();
            return std::unexpected(last_err);
        }
        // Retryable: backoff. Tmp still on disk → -C - resumes next attempt.
        if (attempt < retries) {
            int wait = 1 << (attempt - 1);
            log::warnf("download attempt {} failed ({}); retry in {}s",
                       attempt, last_err.message, wait);
            std::this_thread::sleep_for(std::chrono::seconds(wait));
        }
    }
    prog.abandon();
    fs::remove(tmp, ec);
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

        std::optional<luban::progress::Bar> prog;
        auto sink = [&](const unsigned char* data, size_t n, int64_t total)
            -> std::expected<void, Error> {
            if (!prog) {
                prog.emplace(luban::progress::Action::fetch(), total,
                             luban::progress::Unit::Bytes, label);
            }
            if (std::fwrite(data, 1, n, fp) != n) {
                return std::unexpected(Error{ErrorKind::Io, "fwrite failed"});
            }
            prog->update(n);
            return {};
        };

        auto rc = do_request(url, opts.timeout_seconds, sink);
        std::fclose(fp);

        if (!rc.has_value()) {
            if (prog) prog->abandon();
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

        if (prog) prog->finish_done();
        // No streaming sha on POSIX yet — verify_and_promote recomputes
        // by re-reading. Future: parallel POSIX StreamingSha256 via EVP.
        auto promoted = verify_and_promote(tmp, dest, opts.expected_hash, url);
        if (!promoted) return std::unexpected(promoted.error());
        return DownloadResult{*promoted, *rc};
    }
    return std::unexpected(last_err);
#endif
}

std::expected<std::string, Error> fetch_text(const std::string& url_in, int timeout_seconds) {
    const std::string url = apply_mirror(url_in);
#ifdef _WIN32
    auto rc = luban::curl_subprocess::fetch_text(url, timeout_seconds);
    if (!rc) {
        const auto& e = rc.error();
        ErrorKind k = ErrorKind::Network;
        if (e.kind == luban::curl_subprocess::ErrorKind::HttpClient)
            k = ErrorKind::HttpClient;
        else if (e.kind == luban::curl_subprocess::ErrorKind::HttpServer)
            k = ErrorKind::HttpServer;
        else if (e.kind == luban::curl_subprocess::ErrorKind::Io)
            k = ErrorKind::Io;
        return std::unexpected(Error{k, e.message});
    }
    return *rc;
#else
    // POSIX: in-process libcurl via the existing do_request template.
    std::string buf;
    auto sink = [&](const unsigned char* data, size_t n, int64_t /*total*/)
        -> std::expected<void, Error> {
        buf.append(reinterpret_cast<const char*>(data), n);
        return {};
    };
    auto rc = do_request(url, timeout_seconds, sink);
    if (!rc.has_value()) return std::unexpected(rc.error());
    return buf;
#endif
}

}  // namespace luban::download
