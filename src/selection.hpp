#pragma once
// 1:1 port of luban_boot/selection.py.
// 读 <config>/selection.json；首次缺失就从仓内 manifests_seed/selection.json 复制过来。

#include <filesystem>
#include <string>
#include <vector>

namespace luban::selection {

namespace fs = std::filesystem;

struct Entry {
    std::string name;
    bool enabled;
    std::string note;        // 来自 _why 字段
};

struct Selection {
    std::vector<Entry> components;
    std::vector<Entry> extras;
};

// 读取项目级 selection.json；deploy_seed=true 时若用户配置缺失，
// 会复制 manifests_seed/selection.json 到 <config>/selection.json。
Selection load(bool deploy_seed = true);

// 把 manifests_seed/*.json（除 selection.json）复制到 <data>/registry/overlay/。
// 仅复制还不存在的，不覆盖用户的 overlay。返回最终 overlay 目录里的全部 .json。
std::vector<fs::path> deploy_overlays();

}  // namespace luban::selection
