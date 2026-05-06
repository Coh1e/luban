// Network / archive half of the store module. Pulls in the heavy
// dependency surface (download.cpp + archive.cpp + hash.cpp + nlohmann
// json), so kept separate from store.cpp's identity/lookup helpers
// which the unit tests link against.
//
// See `store.hpp` for design rationale; relevant section is the
// "atomic install via tmp + rename" comment.

#include "store.hpp"

#include <chrono>
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
    fs::path final_dir = store_path(artifact_id);
    if (is_present(artifact_id)) {
        return FetchResult{final_dir, final_dir / std::string(bin), true};
    }

    std::error_code ec;
    fs::create_directories(store_root(), ec);
    if (ec) return std::unexpected("cannot create store root: " + ec.message());

    // Step 1: download.
    fs::path cache_downloads = paths::cache_dir() / "downloads";
    fs::create_directories(cache_downloads, ec);
    if (ec) return std::unexpected("cannot create cache/downloads: " + ec.message());
    fs::path archive_path = cache_downloads / (std::string(artifact_id) + ".archive");

    auto hash_spec = hash::parse(sha256);
    if (!hash_spec) {
        return std::unexpected("invalid sha256 spec: " + std::string(sha256));
    }

    download::DownloadOptions dlopts;
    dlopts.expected_hash = *hash_spec;
    dlopts.label = opts.label.empty() ? std::string(artifact_id) : opts.label;
    auto dl = download::download(std::string(url), archive_path, dlopts);
    if (!dl) {
        return std::unexpected("download failed: " + dl.error().message);
    }

    // Step 2: extract to tmp dir.
    fs::path tmp = tmp_extract_dir(artifact_id);
    fs::create_directories(tmp, ec);
    if (ec) {
        return std::unexpected("cannot create tmp extract dir: " + ec.message());
    }

    auto extract = archive::extract(archive_path, tmp);
    if (!extract) {
        fs::remove_all(tmp, ec);
        return std::unexpected("extract failed: " + extract.error().message);
    }

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
    fs::rename(tmp, final_dir, ec);
    if (ec) {
        if (is_present(artifact_id)) {
            fs::remove_all(tmp, ec);
            return FetchResult{final_dir, final_dir / std::string(bin), true};
        }
        std::error_code ec2;
        fs::remove_all(final_dir, ec2);
        fs::rename(tmp, final_dir, ec2);
        if (ec2) {
            fs::remove_all(tmp, ec);
            return std::unexpected("rename to final dir failed: " + ec2.message());
        }
    }

    return FetchResult{final_dir, final_dir / std::string(bin), false};
}

}  // namespace luban::store
