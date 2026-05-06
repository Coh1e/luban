#include "path_search.hpp"

#include <array>
#include <cstdlib>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#include "util/win.hpp"
#endif

namespace luban::path_search {

std::optional<fs::path> on_path(std::string_view tool) {
#ifdef _WIN32
    std::wstring wname = win::from_utf8(tool);
    // SearchPathW handles PATHEXT-style fallback only when the bare name has
    // no extension. Iterate the common executable extensions explicitly so
    // luban's `.cmd` shims match too — `cmake.cmd` would otherwise be
    // skipped by a naked SearchPathW(L"cmake").
    wchar_t buf[MAX_PATH * 4] = {};
    LPWSTR fp = nullptr;
    static const std::array<const wchar_t*, 5> exts = {L".exe", L".cmd", L".bat", L".com", L""};
    for (auto ext : exts) {
        std::wstring trial = wname;
        if (ext[0] != 0) trial += ext;
        DWORD got = SearchPathW(nullptr, trial.c_str(), nullptr,
                                static_cast<DWORD>(std::size(buf)), buf, &fp);
        if (got > 0 && got < std::size(buf)) {
            return fs::path(win::to_utf8(std::wstring(buf, got)));
        }
    }
    return std::nullopt;
#else
    const char* path = std::getenv("PATH");
    if (!path) return std::nullopt;
    std::string p(path);
    size_t start = 0;
    while (start <= p.size()) {
        size_t end = p.find(':', start);
        if (end == std::string::npos) end = p.size();
        fs::path candidate = fs::path(p.substr(start, end - start)) / std::string(tool);
        std::error_code ec;
        if (fs::exists(candidate, ec)) return candidate;
        if (end == p.size()) break;
        start = end + 1;
    }
    return std::nullopt;
#endif
}

}  // namespace luban::path_search
