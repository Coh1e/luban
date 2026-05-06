#pragma once
// Win32 utilities: utf8 ↔ wstring conversions for paths/env vars/argv.
// Plan §7: paths are NOT canonicalized — junctions like D:\dobby → C:\Users\Rust
// must surface as the literal string the user typed.

#include <string>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#endif

namespace luban::win {

#ifdef _WIN32

inline std::wstring from_utf8(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), n);
    return out;
}

inline std::string to_utf8(std::wstring_view s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

#endif

}  // namespace luban::win
