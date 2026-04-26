#include "env_snapshot.hpp"

#include <algorithm>
#include <cstdlib>
#include <set>
#include <string>

#include "paths.hpp"
#include "registry.hpp"

namespace luban::env_snapshot {

namespace {

#ifdef _WIN32
constexpr char kPathSep = ';';
#else
constexpr char kPathSep = ':';
#endif

std::string lower(const std::string& s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

}  // namespace

std::vector<fs::path> path_dirs() {
    std::set<std::string> seen;
    std::vector<fs::path> ordered;
    auto add = [&](const fs::path& p) {
        std::error_code ec;
        if (!fs::exists(p, ec)) return;
#ifdef _WIN32
        std::string key = lower(p.string());
#else
        std::string key = p.string();
#endif
        if (!seen.insert(key).second) return;
        ordered.push_back(p);
    };

    auto recs = registry::load_installed();
    for (auto& [_, rec] : recs) {
        fs::path root = paths::toolchain_dir(rec.toolchain_dir);
        for (auto& [alias, rel] : rec.bins) {
            // Normalize separators — bin entries are stored with '/' on disk.
            std::string norm = rel;
            for (auto& c : norm) if (c == '/' || c == '\\') c = static_cast<char>(fs::path::preferred_separator);
            fs::path exe = root / norm;
            add(exe.parent_path());
        }
    }
    add(paths::bin_dir());
    return ordered;
}

std::vector<std::pair<std::string, std::string>> env_dict() {
    return {
        {"LUBAN_DATA",   paths::data_dir().string()},
        {"LUBAN_CACHE",  paths::cache_dir().string()},
        {"LUBAN_STATE",  paths::state_dir().string()},
        {"LUBAN_CONFIG", paths::config_dir().string()},
    };
}

std::map<std::string, std::string> apply_to(const std::map<std::string, std::string>& env) {
    std::map<std::string, std::string> out;
    auto extras = path_dirs();
    if (!extras.empty()) {
        std::string joined;
        for (size_t i = 0; i < extras.size(); ++i) {
            if (i) joined.push_back(kPathSep);
            joined += extras[i].string();
        }
        // 找 existing PATH：先看 env 参数；没有就读当前进程 env。
        // 这一行很关键 — 否则 child cmake 看不到 C:\Windows\System32 之类系统路径，
        // vcpkg/cmake 调 ninja 等会找不到 build program。
        std::string existing;
        auto it = env.find("PATH");
        if (it != env.end()) {
            existing = it->second;
        } else if (const char* p = std::getenv("PATH"); p && *p) {
            existing = p;
        }
        out["PATH"] = existing.empty() ? joined : (joined + kPathSep + existing);
    }
    for (auto& [k, v] : env_dict()) out[k] = v;
    return out;
}

}  // namespace luban::env_snapshot
