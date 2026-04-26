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

// 写回 <config>/selection.json (atomic tmp+rename)。
// 用于 `luban setup --with X` / `--without X` 持久化用户选择。
// 写出格式：components / extras 两段，与 seed schema 1 一致。
void save(const Selection& sel);

// 把 X 加到 selection 的 extras 里（若已存在则只置 enabled=true）。
// 写不存在 / disabled 入口时，note 会留空。返回 true 表示发生了变更。
bool enable(Selection& sel, const std::string& name);

// 把 X 在 selection 中置为 disabled。返回 true 表示发生了变更（即原本是 enabled）。
bool disable(Selection& sel, const std::string& name);

}  // namespace luban::selection
