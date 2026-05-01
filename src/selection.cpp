#include "selection.hpp"

#include <fstream>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#include "util/win.hpp"
#endif

#include "file_util.hpp"
#include "json.hpp"
#include "log.hpp"
#include "paths.hpp"

namespace luban::selection {

namespace {

using nlohmann::json;

// 找仓里的 manifests_seed 目录。Python 退场后，seed 在仓根 manifests_seed/。
// 候选（按优先级）：
// 1) <exe_dir>/manifests_seed                     （安装后并排，未来 luban-init.exe）
// 2) <exe_dir>/../manifests_seed                  （build/luban.exe → 仓根）
// 3) <exe_dir>/../../manifests_seed               （build/default/luban.exe → 仓根）
fs::path seed_root() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4];
    DWORD got = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    fs::path exe = (got == 0) ? fs::current_path() / "luban.exe" : fs::path(std::wstring(buf, got));
#else
    fs::path exe = fs::current_path() / "luban";
#endif
    fs::path d = exe.parent_path();
    std::vector<fs::path> candidates = {
        d / "manifests_seed",
        d.parent_path() / "manifests_seed",
        d.parent_path().parent_path() / "manifests_seed",
    };
    std::error_code ec;
    for (auto& c : candidates) {
        if (fs::is_directory(c, ec)) return c;
    }
    return {};
}

std::vector<Entry> coerce(const json& arr) {
    std::vector<Entry> out;
    if (!arr.is_array()) return out;
    for (auto& item : arr) {
        if (!item.is_object()) continue;
        if (!item.contains("name") || !item["name"].is_string()) continue;
        std::string name = item["name"].get<std::string>();
        if (name.empty()) continue;
        bool enabled = true;
        if (item.contains("enabled") && item["enabled"].is_boolean()) {
            enabled = item["enabled"].get<bool>();
        }
        std::string note;
        if (item.contains("_why") && item["_why"].is_string()) {
            note = item["_why"].get<std::string>();
        }
        out.push_back({name, enabled, note});
    }
    return out;
}

}  // namespace

Selection load(bool deploy_seed) {
    fs::path target = paths::selection_json_path();
    fs::path seed = seed_root() / "selection.json";

    std::error_code ec;
    if (!fs::exists(target, ec)) {
        if (deploy_seed && fs::exists(seed, ec)) {
            // Copy the in-tree seed to the user's config dir on first
            // run. file_util::write_text_atomic handles parent-dir
            // creation + tmp+rename — atomic write is overkill for a
            // first-time copy but harmless and dedups the I/O surface.
            std::string text = file_util::read_text_no_bom(seed);
            file_util::write_text_atomic(target, text);
        } else {
            // 不部署 seed 也不存在用户文件——直接读 seed 当源
            target = seed;
        }
    }

    Selection sel;
    if (!fs::exists(target, ec)) return sel;

    std::string text = file_util::read_text_no_bom(target);
    json doc;
    try { doc = json::parse(text); } catch (...) { return sel; }
    if (!doc.is_object()) return sel;
    if (doc.contains("components")) sel.components = coerce(doc["components"]);
    if (doc.contains("extras"))     sel.extras = coerce(doc["extras"]);
    return sel;
}

namespace {

json entry_to_json(const Entry& e) {
    json j = json::object();
    j["name"] = e.name;
    j["enabled"] = e.enabled;
    if (!e.note.empty()) j["_why"] = e.note;
    return j;
}

}  // namespace

void save(const Selection& sel) {
    json doc = json::object();
    doc["_comment"] = "Luban toolchain selection. Edited by `luban setup --with` / `--without`. See manifests_seed/selection.json for the original seed.";
    doc["schema"] = 1;
    doc["components"] = json::array();
    for (auto& e : sel.components) doc["components"].push_back(entry_to_json(e));
    doc["extras"] = json::array();
    for (auto& e : sel.extras) doc["extras"].push_back(entry_to_json(e));

    fs::path target = paths::selection_json_path();
    file_util::write_text_atomic(target, doc.dump(2));
}

bool enable(Selection& sel, const std::string& name) {
    for (auto* list : {&sel.components, &sel.extras}) {
        for (auto& e : *list) {
            if (e.name == name) {
                if (e.enabled) return false;
                e.enabled = true;
                return true;
            }
        }
    }
    Entry fresh;
    fresh.name = name;
    fresh.enabled = true;
    sel.extras.push_back(std::move(fresh));
    return true;
}

bool disable(Selection& sel, const std::string& name) {
    for (auto* list : {&sel.components, &sel.extras}) {
        for (auto& e : *list) {
            if (e.name == name) {
                if (!e.enabled) return false;
                e.enabled = false;
                return true;
            }
        }
    }
    return false;
}

std::vector<fs::path> deploy_overlays() {
    fs::path seed = seed_root();
    fs::path overlay = paths::overlay_dir();
    std::vector<fs::path> deployed;

    std::error_code ec;
    fs::create_directories(overlay, ec);
    if (seed.empty() || !fs::is_directory(seed, ec)) return deployed;

    for (auto& entry : fs::directory_iterator(seed, ec)) {
        if (!entry.is_regular_file()) continue;
        const auto& src = entry.path();
        if (src.extension() != ".json") continue;
        if (src.filename() == "selection.json") continue;

        fs::path dst = overlay / src.filename();
        if (!fs::exists(dst, ec)) {
            std::string text = file_util::read_text_no_bom(src);
            file_util::write_text_atomic(dst, text);
        }
        deployed.push_back(dst);
    }
    return deployed;
}

}  // namespace luban::selection
