#include "path_search.hpp"

#include <array>
#include <cstdlib>
#include <string>
#include <system_error>

#include <windows.h>
#include "util/win.hpp"

namespace luban::path_search {

std::optional<fs::path> on_path(std::string_view tool) {
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
}

}  // namespace luban::path_search
