// Network / archive half of the store module. Pulls in the heavy
// dependency surface (download.cpp + archive.cpp + hash.cpp + nlohmann
// json), so kept separate from store.cpp's identity/lookup helpers
// which the unit tests link against.
//
// See `store.hpp` for design rationale; relevant section is the
// "atomic install via tmp + rename" comment.

#include "store.hpp"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#include <process.h>  // _getpid
#else
#include <unistd.h>   // getpid
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
    // Multi-stream Range download. A single TCP connection to GitHub
    // releases gets per-connection-throttled by the CDN, especially on
    // slow / high-latency links (CN, VN). Splitting into N parallel
    // chunks aggregates closer to link ceiling — typically 2-3x for
    // ~200 MB artifacts. Falls back cleanly to single-stream when the
    // server can't slice or the file is below threshold (8 MiB).
    //
    // Override via LUBAN_PARALLEL_CHUNKS=N (0 = disable) for users who
    // want to crank further on high-latency links.
    dlopts.parallel_chunks = 4;
    if (const char* env = std::getenv("LUBAN_PARALLEL_CHUNKS")) {
        try { dlopts.parallel_chunks = std::stoi(env); } catch (...) {}
    }
    auto dl = download::download(std::string(url), archive_path, dlopts);
    if (!dl) {
        return std::unexpected("download failed (url=" + std::string(url) +
                               "): " + dl.error().message);
    }

    // Step 2: extract to tmp dir.
    log::infof("  extracting {}...", archive_path.filename().string());
    fs::path tmp = tmp_extract_dir(artifact_id);
    fs::create_directories(tmp, ec);
    if (ec) {
        return std::unexpected("cannot create tmp extract dir " +
                               tmp.string() + ": " + ec.message());
    }

    auto t_extract = std::chrono::steady_clock::now();
    auto extract = archive::extract(archive_path, tmp);
    if (!extract) {
        fs::remove_all(tmp, ec);
        return std::unexpected("extract failed (archive=" + archive_path.string() +
                               "): " + extract.error().message);
    }
    auto dt_extract = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t_extract).count();
    log::infof("  extracted in {:.1f}s", dt_extract);

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
