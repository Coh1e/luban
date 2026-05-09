#include "env_snapshot.hpp"

#include <algorithm>
#include <cstdlib>
#include <set>
#include <string>

#include "msvc_env.hpp"
#include "paths.hpp"

namespace luban::env_snapshot {

namespace {

constexpr char kPathSep = ';';

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
        std::string key = lower(p.string());
        if (!seen.insert(key).second) return;
        ordered.push_back(p);
    };

    // Post-MVP (DESIGN §2 #5): the only bin dir luban ever puts on PATH is
    // ~/.local/bin. Tools installed via blueprint live in
    // ~/.local/share/luban/store/<artifact_id>/ but are exposed to users
    // exclusively through .cmd shims at ~/.local/bin/<alias>.cmd, so we
    // don't walk the store dir to add toolchain bin paths anymore.
    add(paths::xdg_bin_home());

    // MSVC tool dirs (when --msvc-init has been run). Without these,
    // luban-spawned children wouldn't be able to resolve cl.exe / link.exe
    // via PATH. Splitting path_addition by ';' (Windows separator) into
    // individual entries.
    if (auto cap = msvc_env::load(); cap && !cap->path_addition.empty()) {
        std::string s = cap->path_addition;
        size_t start = 0;
        while (start <= s.size()) {
            size_t end = s.find(';', start);
            if (end == std::string::npos) end = s.size();
            std::string dir = s.substr(start, end - start);
            if (!dir.empty()) add(fs::path(dir));
            if (end == s.size()) break;
            start = end + 1;
        }
    }

    return ordered;
}

std::vector<std::pair<std::string, std::string>> env_dict() {
    // Vars luban spawn-children should see beyond plain PATH:
    //
    //   VCPKG_DOWNLOADS / VCPKG_DEFAULT_BINARY_CACHE / X_VCPKG_REGISTRIES_CACHE
    //                Route vcpkg's own caches into <cache>/vcpkg/* instead
    //                of vcpkg's scattered defaults. One place to clear
    //                everything.
    //
    // VCPKG_ROOT and EM_CONFIG used to be inferred from the registry of
    // installed components (v0.x model). With registry/installed.json gone
    // (DESIGN §11), those are now the user's responsibility — set them in
    // HKCU directly (or rely on luban-managed shims that don't need them).
    std::vector<std::pair<std::string, std::string>> out;

    // vcpkg insists the cache dirs exist before it touches them. Create-
    // on-emit so users get a working `luban build` even without an
    // explicit setup step.
    {
        std::error_code ec;
        fs::create_directories(paths::vcpkg_downloads_dir(), ec);
        fs::create_directories(paths::vcpkg_archives_dir(), ec);
        fs::create_directories(paths::vcpkg_registries_dir(), ec);
    }
    out.emplace_back("VCPKG_DOWNLOADS",            paths::vcpkg_downloads_dir().string());
    out.emplace_back("VCPKG_DEFAULT_BINARY_CACHE", paths::vcpkg_archives_dir().string());
    out.emplace_back("X_VCPKG_REGISTRIES_CACHE",   paths::vcpkg_registries_dir().string());

    // MSVC: if the user ran `luban env --msvc-init`, the captured INCLUDE /
    // LIB / LIBPATH / WindowsSdk* / VCToolsInstallDir / etc. live in
    // <state>/msvc-env.json. Merge them so luban-spawned cmake can drive
    // cl.exe without the user sourcing vcvarsall first. PATH addition is
    // handled separately in apply_to() so it merges cleanly with luban's
    // own PATH overlay.
    if (auto cap = msvc_env::load()) {
        for (auto& [k, v] : cap->vars) out.emplace_back(k, v);
    }

    return out;
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
