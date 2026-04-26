#pragma once
// 1:1 port of luban_boot/bucket_sync.py 的 fetch_manifest 路径。
// Full-mirror（refresh_full_mirrors）需要 ZIP 解压（miniz），延后到 archive 模块就绪后补。
//
// 查找顺序：
//   1) <data>/registry/overlay/<app>.json   ← 优先（手写覆盖）
//   2) 缓存命中：<data>/registry/buckets/<bucket>/bucket/<app>.json
//   3) 远程 GET：raw.githubusercontent.com/<repo>/<branch>/bucket/<app>.json，写入缓存

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "json.hpp"

namespace luban::bucket_sync {

namespace fs = std::filesystem;

struct BucketInfo {
    std::string name;        // 缓存子目录名，如 "scoop-main"
    std::string repo;        // GitHub owner/repo
    std::string branch;
};

// 默认两个上游 bucket（与 Python 端一致）。
const std::vector<BucketInfo>& buckets();

struct FetchResult {
    fs::path manifest_path;       // 命中后的本地路径
    nlohmann::json manifest;      // 解析好的 JSON
    std::string source_label;     // "overlay" / "cache:scoop-main" / "remote:scoop-main"
};

// 查找并解析 `app` 的 manifest。失败返回 nullopt。
std::optional<FetchResult> fetch_manifest(const std::string& app, bool prefer_overlay = true,
                                          bool force_refresh = false);

}  // namespace luban::bucket_sync
