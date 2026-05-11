// See `libcurl_backend.hpp` for design rationale.

#include "libcurl_backend.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include <curl/curl.h>

#include "log.hpp"
#include "luban/version.hpp"

namespace luban::libcurl_backend {

namespace {

namespace fs = std::filesystem;

// ---- global init ----------------------------------------------------

std::once_flag g_init_flag;
std::atomic<bool> g_init_ok{false};

void do_init() {
    CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
    g_init_ok.store(rc == CURLE_OK, std::memory_order_release);
}

// ---- common ---------------------------------------------------------

// User-Agent: matches the subprocess driver's UA so server-side logs /
// allowlists stay consistent across the backend swap.
std::string user_agent() {
    return std::string("luban/") +
           std::string(luban::kLubanVersion) +
           std::string(" (+https://github.com/Coh1e/luban)");
}

struct XferContext {
    luban::progress::Bar* prog = nullptr;
    curl_off_t last_reported = 0;  ///< Bytes already passed to Bar::update
};

// CURLOPT_XFERINFOFUNCTION: drives the progress bar with the real
// over-the-wire byte counter from libcurl. Called on libcurl's internal
// schedule (~200ms by default; tweakable via CURLOPT_PROGRESSFUNCTION_INTERVAL
// in newer libcurl but unnecessary here). Returns 0 to continue; non-zero
// to abort with CURLE_ABORTED_BY_CALLBACK.
int xferinfo_cb(void* clientp, curl_off_t /*dltotal*/, curl_off_t dlnow,
                curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
    auto* ctx = static_cast<XferContext*>(clientp);
    if (ctx && ctx->prog && dlnow > ctx->last_reported) {
        ctx->prog->update(static_cast<std::size_t>(dlnow - ctx->last_reported));
        ctx->last_reported = dlnow;
    }
    return 0;
}

// Default WRITEFUNCTION (fwrite) writes to the FILE* set via WRITEDATA,
// so the file-download path doesn't need a custom callback. The text
// path below uses a small std::string sink.
size_t write_to_string_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    size_t bytes = size * nmemb;
    s->append(ptr, bytes);
    return bytes;
}

// ---- error classification ------------------------------------------

// Map a CURLcode + (optional) HTTP status to our public ErrorKind.
// Mirrors the curl_subprocess driver's classification table so existing
// retry behavior in download.cpp is preserved.
ErrorKind classify(CURLcode rc, long http_status, double speed_dl, double total_time) {
    if (rc == CURLE_HTTP_RETURNED_ERROR) {
        // CURLOPT_FAILONERROR yields this on 4xx/5xx. The CURLcode alone
        // doesn't tell us which; consult the response code we already
        // fetched via getinfo.
        if (http_status >= 500) return ErrorKind::HttpServer;
        return ErrorKind::HttpClient;
    }
    if (rc == CURLE_OPERATION_TIMEDOUT) {
        // Could be CURLOPT_TIMEOUT (max_time hit) or CURLOPT_LOW_SPEED_*.
        // Distinguish: if total_time < max_time and speed is below the
        // stall threshold, treat as Stalled (tainted partial — delete);
        // otherwise as Network (keep partial for resume).
        if (speed_dl < static_cast<double>(kStallMinBytesPerSec) && total_time > 0.0) {
            return ErrorKind::Stalled;
        }
        return ErrorKind::Network;
    }
    if (rc == CURLE_WRITE_ERROR) return ErrorKind::Io;
    // Everything else (resolve, connect, TLS, recv, h2 stream …) is
    // retriable transport noise.
    return ErrorKind::Network;
}

// Apply the common easy-handle options shared by all entry points
// (download / fetch_text / head_content_length).
void apply_common_opts(CURL* h, const std::string& url, int connect_to, int max_to) {
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_USERAGENT, user_agent().c_str());
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 20L);
    curl_easy_setopt(h, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, static_cast<long>(connect_to));
    curl_easy_setopt(h, CURLOPT_TIMEOUT, static_cast<long>(max_to));
    // Force HTTP/2 over TLS — the headline win of going off System32 curl.
    // Falls back to 1.1 if the server doesn't ALPN-advertise h2 (rare on
    // GitHub / Fastly / Cloudflare endpoints luban hits).
    curl_easy_setopt(h, CURLOPT_HTTP_VERSION,
                     static_cast<long>(CURL_HTTP_VERSION_2TLS));
    curl_easy_setopt(h, CURLOPT_BUFFERSIZE, 262144L);  // 256 KiB
    curl_easy_setopt(h, CURLOPT_TCP_NODELAY, 1L);
    // Schannel uses the Windows certificate store; default verify
    // settings (peer + host) are correct.
}

}  // namespace

void ensure_global_init() {
    std::call_once(g_init_flag, do_init);
}

std::expected<Result, Error> download_to_file(
    const std::string& url, const fs::path& dest, const Options& opts) {
    ensure_global_init();
    if (!g_init_ok.load(std::memory_order_acquire)) {
        return std::unexpected(Error{ErrorKind::Io,
            "curl_global_init failed", 0, 0});
    }

    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);

    // Resume sizing: if dest exists, seed RESUME_FROM_LARGE so the server
    // returns 206 Partial Content. fopen mode tracks: "ab" appends to the
    // existing partial, "wb" truncates a fresh download. Either way, the
    // file ends up holding the complete payload on success.
    std::int64_t before = 0;
    if (opts.resume && fs::exists(dest, ec)) {
        before = static_cast<std::int64_t>(fs::file_size(dest, ec));
        if (ec) before = 0;
    }
    const char* mode = (before > 0) ? "ab" : "wb";
    std::FILE* f = nullptr;
    if (fopen_s(&f, dest.string().c_str(), mode) != 0 || !f) {
        return std::unexpected(Error{ErrorKind::Io,
            "open dest for write failed: " + dest.string(), 0, 0});
    }

    CURL* h = curl_easy_init();
    if (!h) {
        std::fclose(f);
        return std::unexpected(Error{ErrorKind::Io,
            "curl_easy_init failed", 0, 0});
    }

    apply_common_opts(h, url, opts.connect_timeout_seconds, opts.max_time_seconds);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, f);  // default WRITEFUNCTION = fwrite
    if (before > 0) {
        curl_easy_setopt(h, CURLOPT_RESUME_FROM_LARGE,
                         static_cast<curl_off_t>(before));
    }

    XferContext xctx;
    xctx.prog = opts.progress;
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, xferinfo_cb);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, &xctx);

    // Stall watchdog: <1 KiB/s sustained over 15s → CURLE_OPERATION_TIMEDOUT.
    // classify() distinguishes this from a max_time hit using speed_dl.
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, kStallMinBytesPerSec);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME,
                     static_cast<long>(kStallWindowSeconds));

    char errbuf[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);

    CURLcode rc = curl_easy_perform(h);

    long http_status = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &http_status);
    double speed_dl = 0.0, total_time = 0.0;
    curl_easy_getinfo(h, CURLINFO_SPEED_DOWNLOAD, &speed_dl);
    curl_easy_getinfo(h, CURLINFO_TOTAL_TIME, &total_time);

    curl_easy_cleanup(h);
    std::fclose(f);

    if (rc != CURLE_OK) {
        ErrorKind k = classify(rc, http_status, speed_dl, total_time);
        std::string msg = errbuf[0] ? std::string(errbuf) : curl_easy_strerror(rc);
        // HttpClient/Server + Stalled paths leave a tainted dest; remove it.
        // Network errors keep the partial so the next attempt resumes.
        if (k == ErrorKind::HttpClient || k == ErrorKind::HttpServer ||
            k == ErrorKind::Stalled) {
            std::error_code ec_rm; fs::remove(dest, ec_rm);
        }
        return std::unexpected(Error{k, std::move(msg),
                                     static_cast<int>(rc), http_status});
    }

    std::int64_t after = static_cast<std::int64_t>(fs::file_size(dest, ec));
    if (ec) {
        return std::unexpected(Error{ErrorKind::Io, "stat dest failed",
                                     0, http_status});
    }
    return Result{after - before, after};
}

std::expected<std::string, Error> fetch_text(
    const std::string& url, int timeout_seconds) {
    ensure_global_init();
    if (!g_init_ok.load(std::memory_order_acquire)) {
        return std::unexpected(Error{ErrorKind::Io,
            "curl_global_init failed", 0, 0});
    }

    CURL* h = curl_easy_init();
    if (!h) {
        return std::unexpected(Error{ErrorKind::Io,
            "curl_easy_init failed", 0, 0});
    }

    int connect_to = std::min(timeout_seconds, 30);
    apply_common_opts(h, url, connect_to, timeout_seconds);

    std::string body;
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_to_string_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &body);

    char errbuf[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);

    CURLcode rc = curl_easy_perform(h);

    long http_status = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &http_status);
    double speed_dl = 0.0, total_time = 0.0;
    curl_easy_getinfo(h, CURLINFO_SPEED_DOWNLOAD, &speed_dl);
    curl_easy_getinfo(h, CURLINFO_TOTAL_TIME, &total_time);

    curl_easy_cleanup(h);

    if (rc != CURLE_OK) {
        ErrorKind k = classify(rc, http_status, speed_dl, total_time);
        std::string msg = errbuf[0] ? std::string(errbuf) : curl_easy_strerror(rc);
        return std::unexpected(Error{k, std::move(msg),
                                     static_cast<int>(rc), http_status});
    }
    return body;
}

std::optional<std::int64_t> head_content_length(
    const std::string& url, int timeout_seconds) {
    ensure_global_init();
    if (!g_init_ok.load(std::memory_order_acquire)) return std::nullopt;

    CURL* h = curl_easy_init();
    if (!h) return std::nullopt;

    int connect_to = std::min(timeout_seconds, 30);
    apply_common_opts(h, url, connect_to, timeout_seconds);
    curl_easy_setopt(h, CURLOPT_NOBODY, 1L);  // HEAD

    CURLcode rc = curl_easy_perform(h);

    std::optional<std::int64_t> out;
    if (rc == CURLE_OK) {
        curl_off_t cl = -1;
        curl_easy_getinfo(h, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);
        if (cl > 0) out = static_cast<std::int64_t>(cl);
    }

    curl_easy_cleanup(h);
    return out;
}

}  // namespace luban::libcurl_backend
