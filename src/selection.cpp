#include "selection.hpp"

#include <fstream>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#include "util/win.hpp"
#endif

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

void write_text(const fs::path& path, const std::string& content) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
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
            std::error_code ec2;
            fs::create_directories(target.parent_path(), ec2);
            std::string text = read_text_strip_bom(seed);
            write_text(target, text);
        } else {
            // 不部署 seed 也不存在用户文件——直接读 seed 当源
            target = seed;
        }
    }

    Selection sel;
    if (!fs::exists(target, ec)) return sel;

    std::string text = read_text_strip_bom(target);
    json doc;
    try { doc = json::parse(text); } catch (...) { return sel; }
    if (!doc.is_object()) return sel;
    if (doc.contains("components")) sel.components = coerce(doc["components"]);
    if (doc.contains("extras"))     sel.extras = coerce(doc["extras"]);
    return sel;
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
            std::string text = read_text_strip_bom(src);
            write_text(dst, text);
        }
        deployed.push_back(dst);
    }
    return deployed;
}

}  // namespace luban::selection
