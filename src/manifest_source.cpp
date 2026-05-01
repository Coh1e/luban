#include "manifest_source.hpp"

#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include "util/win.hpp"
#endif

#include "log.hpp"
#include "paths.hpp"

namespace luban::manifest_source {

namespace {

using nlohmann::json;

// Locate the in-tree manifests_seed dir. Mirrors selection.cpp::seed_root()
// — kept as a private duplicate rather than coupling the two modules,
// because seed_root is a leaf utility and both modules independently
// resolve it. Refactor candidate if a third caller appears.
//
// Candidate order (by typical install layout):
//   1) <exe_dir>/manifests_seed                      (installed alongside)
//   2) <exe_dir>/../manifests_seed                   (build/ next to repo root)
//   3) <exe_dir>/../../manifests_seed                (build/release/luban.exe)
fs::path seed_root() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4];
    DWORD got = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    fs::path exe = (got == 0) ? fs::current_path() / "luban.exe"
                              : fs::path(std::wstring(buf, got));
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

// Read a JSON file, stripping a leading UTF-8 BOM if present (Scoop / Notepad
// users sometimes save manifests with one). Returns empty string on failure.
std::string read_text_strip_bom(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
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

// Try to parse a single candidate file. Returns nullopt if the file doesn't
// exist or the JSON parse fails. Parse failures are logged at warn level —
// the caller may still succeed via another source.
std::optional<json> try_parse(const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return std::nullopt;
    std::string text = read_text_strip_bom(path);
    try {
        return json::parse(text);
    } catch (const std::exception& e) {
        log::warnf("could not parse {}: {}", path.string(), e.what());
        return std::nullopt;
    }
}

}  // namespace

std::optional<LoadResult> load(const std::string& name) {
    // 1. overlay (user-overridable, populated on first setup from seed)
    fs::path overlay = paths::overlay_dir() / (name + ".json");
    if (auto j = try_parse(overlay)) {
        return LoadResult{std::move(*j), overlay, "overlay"};
    }

    // 2. seed (in-tree manifests_seed/ next to luban.exe). Lets a fresh
    //    LUBAN_PREFIX work without first running selection::deploy_overlays.
    fs::path seed = seed_root();
    if (!seed.empty()) {
        fs::path direct = seed / (name + ".json");
        if (auto j = try_parse(direct)) {
            return LoadResult{std::move(*j), direct, "seed"};
        }
    }

    return std::nullopt;
}

}  // namespace luban::manifest_source
