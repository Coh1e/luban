#include "bucket_sync.hpp"

#include <fstream>
#include <sstream>
#include <system_error>

#include "download.hpp"
#include "log.hpp"
#include "paths.hpp"

namespace luban::bucket_sync {

namespace {

using nlohmann::json;

constexpr const char* kRawTpl = "https://raw.githubusercontent.com/{}/{}/bucket/{}.json";

std::string render_url(const std::string& tpl, const std::string& a,
                       const std::string& b, const std::string& c) {
    std::string out;
    out.reserve(tpl.size() + a.size() + b.size() + c.size());
    int idx = 0;
    for (size_t i = 0; i < tpl.size(); ++i) {
        if (i + 1 < tpl.size() && tpl[i] == '{' && tpl[i+1] == '}') {
            switch (idx++) {
                case 0: out += a; break;
                case 1: out += b; break;
                case 2: out += c; break;
                default: break;
            }
            ++i;
        } else {
            out.push_back(tpl[i]);
        }
    }
    return out;
}

fs::path cache_root_of(const BucketInfo& b) { return paths::buckets_dir() / b.name; }
fs::path manifest_dir_of(const BucketInfo& b) { return cache_root_of(b) / "bucket"; }
fs::path manifest_path_of(const BucketInfo& b, const std::string& app) {
    return manifest_dir_of(b) / (app + ".json");
}

std::string read_text_strip_bom(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF
        && static_cast<unsigned char>(s[1]) == 0xBB
        && static_cast<unsigned char>(s[2]) == 0xBF) {
        s.erase(0, 3);
    }
    return s;
}

std::optional<json> try_parse(const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return std::nullopt;
    try {
        return json::parse(read_text_strip_bom(path));
    } catch (...) {
        log::warnf("could not parse {}", path.string());
        return std::nullopt;
    }
}

}  // namespace

const std::vector<BucketInfo>& buckets() {
    static const std::vector<BucketInfo> v = {
        {"scoop-main",   "ScoopInstaller/Main",   "master"},
        {"scoop-extras", "ScoopInstaller/Extras", "master"},
    };
    return v;
}

std::optional<FetchResult> fetch_manifest(const std::string& app, bool prefer_overlay,
                                          bool force_refresh) {
    std::error_code ec;

    // 1. overlay
    if (prefer_overlay) {
        fs::path ovr = paths::overlay_dir() / (app + ".json");
        if (fs::exists(ovr, ec)) {
            if (auto j = try_parse(ovr)) {
                return FetchResult{ovr, std::move(*j), "overlay"};
            }
        }
    }

    // 2-3. 各 bucket：cache → remote
    for (auto& b : buckets()) {
        fs::path local = manifest_path_of(b, app);

        if (!force_refresh && fs::exists(local, ec)) {
            if (auto j = try_parse(local)) {
                return FetchResult{local, std::move(*j), "cache:" + b.name};
            }
        }

        std::string url = render_url(kRawTpl, b.repo, b.branch, app);
        fs::create_directories(local.parent_path(), ec);

        download::DownloadOptions opts;
        opts.label = b.name + "/" + app + ".json";
        opts.retries = 2;
        opts.timeout_seconds = 15;
        auto rc = download::download(url, local, opts);
        if (!rc) {
            // 4xx 是真没这个 manifest，继续找下个 bucket
            // 其它错也继续，让循环试下个 bucket，最后整体失败
            continue;
        }
        if (auto j = try_parse(local)) {
            return FetchResult{local, std::move(*j), "remote:" + b.name};
        }
    }

    return std::nullopt;
}

}  // namespace luban::bucket_sync
