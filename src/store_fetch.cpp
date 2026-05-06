// Network / archive half of the store module. Pulls in the heavy
// dependency surface (download.cpp + archive.cpp + hash.cpp + nlohmann
// json), so kept separate from store.cpp's identity/lookup helpers
// which the unit tests link against.
//
// See `store.hpp` for design rationale; relevant section is the
// "atomic install via tmp + rename" comment.

#include "store.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <format>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <io.h>       // _isatty / _fileno
#include <process.h>  // _getpid
#else
#include <unistd.h>   // ::isatty / getpid
#endif

#include "json.hpp"

#include "archive.hpp"
#include "download.hpp"
#include "hash.hpp"
#include "log.hpp"
#include "paths.hpp"

namespace luban::store {

namespace {

namespace fs = std::filesystem;

constexpr const char* kMarkerName = ".store-marker.json";

fs::path store_root() { return paths::data_dir() / "store"; }

// Item-counter progress bar for archive::extract. Mirrors download.cpp's
// Progress class (TTY default + LUBAN_PROGRESS / LUBAN_NO_PROGRESS env
// overrides + 100ms render throttle + \r\x1b[2K clear-line) but counts
// extracted files instead of bytes — extract has no global byte total to
// chase, just N entries to enumerate.
//
// Why duplicated rather than extracted to a shared header: the byte vs.
// item formatter divergence + the rest of the class is ~50 lines. Refactor
// to shared `progress.hpp` if a third caller appears.
//
// Thread safety: callbacks fire from N parallel extract workers. The mutex
// serializes render + last-time read; throttling means actual stderr writes
// happen at most ~10 fps even with hundreds of cb invocations per second
// (so contention is negligible — workers block ~1ms per 100ms window).
class ExtractProgress {
public:
    explicit ExtractProgress(std::string lbl)
        : label_(std::move(lbl)),
          t0_(std::chrono::steady_clock::now()), last_(t0_) {
#ifdef _WIN32
        bool tty = _isatty(_fileno(stderr));
#else
        bool tty = ::isatty(2);
#endif
        bool force_on  = std::getenv("LUBAN_PROGRESS") != nullptr;
        bool force_off = std::getenv("LUBAN_NO_PROGRESS") != nullptr;
        enabled_ = (tty || force_on) && !force_off;
    }

    void update(std::size_t done, std::size_t total) {
        if (!enabled_) return;
        std::lock_guard<std::mutex> g(mu_);
        auto now = std::chrono::steady_clock::now();
        bool finished = (total > 0 && done >= total);
        if (!finished) {
            auto dt = std::chrono::duration<double>(now - last_).count();
            if (dt < 0.1) return;
        }
        last_ = now;
        render(now, done, total);
    }

    void finish() {
        if (!enabled_) return;
        std::lock_guard<std::mutex> g(mu_);
        std::fprintf(stderr, "\r\x1b[2K");
        std::fflush(stderr);
    }

private:
    void render(std::chrono::steady_clock::time_point now,
                std::size_t done, std::size_t total) {
        double elapsed = std::chrono::duration<double>(now - t0_).count();
        if (elapsed < 1e-3) elapsed = 1e-3;
        double rate = static_cast<double>(done) / elapsed;
        std::string line;
        if (total > 0) {
            double pct = 100.0 * static_cast<double>(done) / static_cast<double>(total);
            constexpr int W = 24;
            int filled = static_cast<int>(W * static_cast<double>(done) / static_cast<double>(total));
            std::string bar;
            for (int i = 0; i < W; ++i) bar += (i < filled ? "#" : "\xc2\xb7");
            line = std::format("  [{}] {:5.1f}%  {}/{} files  {:.0f}/s  {}",
                               bar, pct, done, total, rate, label_);
        } else {
            line = std::format("  {} files  {:.0f}/s  {}", done, rate, label_);
        }
        std::fprintf(stderr, "\r\x1b[2K%s", line.c_str());
        std::fflush(stderr);
    }

    std::string label_;
    std::chrono::steady_clock::time_point t0_;
    std::chrono::steady_clock::time_point last_;
    std::mutex mu_;
    bool enabled_ = false;
};

fs::path tmp_extract_dir(std::string_view artifact_id) {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    auto suffix = rng() & 0xFFFFFFu;
    std::ostringstream name;
#ifdef _WIN32
    name << ".tmp-" << artifact_id << "-" << ::_getpid() << "-" << std::hex << suffix;
#else
    name << ".tmp-" << artifact_id << "-" << ::getpid() << "-" << std::hex << suffix;
#endif
    return store_root() / name.str();
}

}  // namespace

std::expected<FetchResult, std::string> fetch(std::string_view artifact_id,
                                              std::string_view url,
                                              std::string_view sha256,
                                              std::string_view bin,
                                              const FetchOptions& opts) {
    // Empty artifact_id used to silently degrade `final_dir` to the store
    // root and `archive_path` to literally `.archive` — a stale .lock
    // produced by an early resolver bug exhibited exactly this. Reject
    // upfront with a hint to re-resolve.
    if (artifact_id.empty()) {
        return std::unexpected(
            "artifact_id is empty (stale lock from a buggy resolver). "
            "Run `luban bp apply --update <bp>` to force re-resolve, or "
            "delete the .lock file next to the blueprint and retry.");
    }
    fs::path final_dir = store_path(artifact_id);
    if (is_present(artifact_id)) {
        log::infof("  cached: {}", final_dir.string());
        return FetchResult{final_dir, final_dir / std::string(bin), true};
    }

    std::error_code ec;
    fs::create_directories(store_root(), ec);
    if (ec) {
        return std::unexpected("cannot create store root " +
                               store_root().string() + ": " + ec.message());
    }

    // Step 1: download.
    fs::path cache_downloads = paths::cache_dir() / "downloads";
    fs::create_directories(cache_downloads, ec);
    if (ec) {
        return std::unexpected("cannot create cache/downloads " +
                               cache_downloads.string() + ": " + ec.message());
    }
    fs::path archive_path = cache_downloads / (std::string(artifact_id) + ".archive");

    auto hash_spec = hash::parse(sha256);
    if (!hash_spec) {
        return std::unexpected("invalid sha256 spec: " + std::string(sha256));
    }

    download::DownloadOptions dlopts;
    dlopts.expected_hash = *hash_spec;
    dlopts.label = opts.label.empty() ? std::string(artifact_id) : opts.label;
    // Single-stream by default. Empirical (VN→github.com, 2026-05-06):
    //   1 connection → 4.7 MB/s
    //   4 connections → 150 KB/s aggregate (3 of 4 chunks throttled +
    //                  1 connection reset by CDN)
    // GitHub's release CDN per-IP-throttles aggressively when it sees
    // multiple parallel TCP connections to the same asset. Browsers
    // appear fast because HTTP/2 multiplexes inside a single connection,
    // never tripping the throttle. Until WinHTTP HTTP/2 is wired up,
    // single-stream is the right default.
    //
    // LUBAN_PARALLEL_CHUNKS=N opts back into multi-stream for networks
    // where the CDN doesn't throttle (private S3 buckets, internal
    // mirrors, etc.). 0 explicitly disables.
    dlopts.parallel_chunks = 1;
    if (const char* env = std::getenv("LUBAN_PARALLEL_CHUNKS")) {
        try { dlopts.parallel_chunks = std::stoi(env); } catch (...) {}
    }
    auto dl = download::download(std::string(url), archive_path, dlopts);
    if (!dl) {
        return std::unexpected("download failed (url=" + std::string(url) +
                               "): " + dl.error().message);
    }

    // Step 2: extract to tmp dir.
    fs::path tmp = tmp_extract_dir(artifact_id);
    fs::create_directories(tmp, ec);
    if (ec) {
        return std::unexpected("cannot create tmp extract dir " +
                               tmp.string() + ": " + ec.message());
    }

    auto t_extract = std::chrono::steady_clock::now();
    // ExtractProgress draws a live bar to stderr (TTY by default, override
    // with LUBAN_PROGRESS / LUBAN_NO_PROGRESS). The "extracting <archive>"
    // banner that used to live here is now subsumed by the bar's label —
    // printing both leaves a ghost line above the live progress.
    ExtractProgress prog("extracting " + archive_path.filename().string());
    auto extract = archive::extract(archive_path, tmp,
        [&prog](std::size_t done, std::size_t total) { prog.update(done, total); });
    prog.finish();
    if (!extract) {
        fs::remove_all(tmp, ec);
        return std::unexpected("extract failed (archive=" + archive_path.string() +
                               "): " + extract.error().message);
    }
    auto dt_extract = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t_extract).count();
    log::infof("  extracted {} in {:.1f}s", archive_path.filename().string(), dt_extract);

    // Step 3: marker. Records what we put there for future debugging /
    // GC / version reconciliation. Format is JSON for jq-friendliness.
    nlohmann::json marker = {
        {"schema", 1},
        {"artifact_id", std::string(artifact_id)},
        {"url", std::string(url)},
        {"sha256", std::string(sha256)},
        {"bin", std::string(bin)},
        {"extracted_at", [] {
             auto t = std::chrono::system_clock::to_time_t(
                 std::chrono::system_clock::now());
             std::ostringstream os;
             struct tm gmt;
#ifdef _WIN32
             gmtime_s(&gmt, &t);
#else
             gmtime_r(&t, &gmt);
#endif
             os << std::put_time(&gmt, "%Y-%m-%dT%H:%M:%SZ");
             return os.str();
         }()},
    };
    {
        std::ofstream mf(tmp / kMarkerName);
        if (!mf) {
            fs::remove_all(tmp, ec);
            return std::unexpected("cannot write marker file");
        }
        mf << marker.dump(2);
    }

    // Step 4: atomic rename to final location. If we lose a race
    // (another luban process got there first), accept its work and
    // clean ours up.
    //
    // Robustness on Windows: the bare fs::rename can return
    // ERROR_PATH_NOT_FOUND when (a) final_dir's parent has disappeared,
    // (b) the source tmp has open handles from AV scanning the freshly
    // extracted exes, or (c) we cross a junction boundary that the NT
    // rename op refuses. Handle each case explicitly and surface paths
    // in the error so the user can act on it.
    std::error_code ec_mk;
    fs::create_directories(final_dir.parent_path(), ec_mk);
    if (ec_mk) {
        fs::remove_all(tmp, ec);
        return std::unexpected("cannot create store dir " +
                               final_dir.parent_path().string() + ": " +
                               ec_mk.message());
    }

    fs::rename(tmp, final_dir, ec);
    if (ec) {
        // (1) Race with a concurrent luban that already finished — accept.
        if (is_present(artifact_id)) {
            fs::remove_all(tmp, ec_mk);
            return FetchResult{final_dir, final_dir / std::string(bin), true};
        }
        // (2) final_dir exists but is incomplete (no marker). Clear and retry.
        std::error_code ec_rm;
        fs::remove_all(final_dir, ec_rm);
        fs::create_directories(final_dir.parent_path(), ec_mk);
        std::error_code ec2;
        fs::rename(tmp, final_dir, ec2);
        if (ec2) {
            // (3) Last-ditch: copy + remove. Survives cross-volume / junction
            // boundaries that rename refuses, at the cost of doubling
            // disk traffic for this one fetch.
            std::error_code ec_cp;
            fs::copy(tmp, final_dir,
                     fs::copy_options::recursive |
                         fs::copy_options::overwrite_existing,
                     ec_cp);
            if (ec_cp) {
                fs::remove_all(tmp, ec_rm);
                return std::unexpected(
                    "rename to final dir failed: " + ec2.message() +
                    " (and copy fallback failed: " + ec_cp.message() + ")"
                    " (src=" + tmp.string() + " dst=" + final_dir.string() + ")");
            }
            fs::remove_all(tmp, ec_rm);
        }
    }

    log::infof("  installed: {}", final_dir.string());
    return FetchResult{final_dir, final_dir / std::string(bin), false};
}

}  // namespace luban::store
