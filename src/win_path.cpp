#include "win_path.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <vector>

#include "util/win.hpp"

namespace luban::win_path {

namespace {

#ifdef _WIN32

// HKCU\Environment 的子键名。
constexpr const wchar_t* kEnvKey = L"Environment";
constexpr const wchar_t* kPathName = L"PATH";

// 读 HKCU\Environment\PATH 的当前值 + 类型（REG_SZ 或 REG_EXPAND_SZ）。
// 不存在返回空 + REG_EXPAND_SZ（默认创建类型）。
std::pair<std::wstring, DWORD> read_path_raw() {
    HKEY key = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, kEnvKey, 0, KEY_QUERY_VALUE, &key);
    if (rc != ERROR_SUCCESS) return {L"", REG_EXPAND_SZ};

    DWORD type = 0;
    DWORD size_bytes = 0;
    rc = RegQueryValueExW(key, kPathName, nullptr, &type, nullptr, &size_bytes);
    if (rc != ERROR_SUCCESS || size_bytes == 0) {
        RegCloseKey(key);
        return {L"", REG_EXPAND_SZ};
    }

    std::wstring buf(size_bytes / sizeof(wchar_t), L'\0');
    rc = RegQueryValueExW(key, kPathName, nullptr, &type,
                          reinterpret_cast<LPBYTE>(buf.data()), &size_bytes);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) return {L"", REG_EXPAND_SZ};

    while (!buf.empty() && buf.back() == L'\0') buf.pop_back();
    return {buf, type};
}

bool write_path_raw(const std::wstring& value, DWORD type) {
    HKEY key = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, kEnvKey, 0, KEY_SET_VALUE, &key);
    if (rc != ERROR_SUCCESS) return false;

    DWORD size_bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    rc = RegSetValueExW(key, kPathName, 0, type,
                        reinterpret_cast<const BYTE*>(value.data()), size_bytes);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) return false;

    // 广播：让 cmd / explorer / 新生 PowerShell 拿到新 PATH。
    DWORD_PTR result = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                        reinterpret_cast<LPARAM>(L"Environment"),
                        SMTO_ABORTIFHUNG, 5000, &result);
    return true;
}

std::vector<std::wstring> split_semi(const std::wstring& s) {
    std::vector<std::wstring> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t end = s.find(L';', start);
        if (end == std::wstring::npos) end = s.size();
        std::wstring piece = s.substr(start, end - start);
        // 跳过空 entry（用户 PATH 末尾经常残留 ;）。
        if (!piece.empty()) out.push_back(piece);
        if (end == s.size()) break;
        start = end + 1;
    }
    return out;
}

std::wstring join_semi(const std::vector<std::wstring>& parts) {
    std::wstring out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out.push_back(L';');
        out += parts[i];
    }
    return out;
}

bool ieq(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::towlower(static_cast<wint_t>(a[i])) !=
            std::towlower(static_cast<wint_t>(b[i]))) return false;
    }
    return true;
}

// 归一化 — 路径分隔符统一成 '\\'，去尾部 '\\'，对照用。
std::wstring normalize(std::wstring s) {
    for (auto& c : s) if (c == L'/') c = L'\\';
    while (s.size() > 3 /*盘符如 C:\*/ && s.back() == L'\\') s.pop_back();
    return s;
}

#endif  // _WIN32

}  // namespace

#ifndef _WIN32
// POSIX implementation of add_to_user_path / remove_from_user_path.
//
// Strategy: append a marker-block to the user's shell rc file
// (~/.bashrc + ~/.zshrc when both exist), with a runtime-idempotent
// `case` guard so re-sourcing the rc doesn't pile up duplicates.
// One block per managed dir; remove looks up by the dir-specific
// marker and deletes the block.
//
// We deliberately don't try to handle fish (different syntax) in v1;
// fish users get a TODO message via log when they hit this code path.

namespace {

const char* kBeginMarker = "# >>> luban-managed PATH: ";
const char* kEndMarker = "# <<< luban-managed PATH <<<";

/// Pick the rc files to update. We always add to ~/.bashrc if it
/// exists or to ~/.profile as a fallback; we ALSO add to ~/.zshrc
/// when present (zsh users typically have both bash and zsh available).
/// Returning multiple files is OK — each file gets its own marker block,
/// and the case-guard inside makes runtime PATH lookups idempotent.
std::vector<fs::path> rc_paths() {
    std::vector<fs::path> out;
    const char* home = std::getenv("HOME");
    if (!home || !*home) return out;
    fs::path h(home);
    std::error_code ec;
    if (fs::is_regular_file(h / ".bashrc", ec)) out.push_back(h / ".bashrc");
    if (fs::is_regular_file(h / ".zshrc", ec))  out.push_back(h / ".zshrc");
    if (out.empty()) {
        // Neither bashrc nor zshrc — fall through to .profile
        // (POSIX default for sh).
        out.push_back(h / ".profile");
    }
    return out;
}

std::string make_block(const fs::path& dir) {
    std::ostringstream ss;
    ss << kBeginMarker << dir.string() << " >>>\n"
       << "case \":$PATH:\" in\n"
       << "  *\":" << dir.string() << ":\"*) ;;\n"
       << "  *) export PATH=\"" << dir.string() << ":$PATH\" ;;\n"
       << "esac\n"
       << kEndMarker << "\n";
    return ss.str();
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p);
    if (!in) return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool write_file(const fs::path& p, const std::string& content) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << content;
    return static_cast<bool>(out);
}

bool block_present(const std::string& content, const fs::path& dir) {
    std::string marker = std::string(kBeginMarker) + dir.string() + " >>>";
    return content.find(marker) != std::string::npos;
}

/// Remove our marker-block for `dir` from `content`. Returns the
/// modified content; the bool is true iff something was removed.
std::pair<std::string, bool> strip_block(const std::string& content,
                                          const fs::path& dir) {
    std::string begin_tag = std::string(kBeginMarker) + dir.string() + " >>>";
    auto begin_pos = content.find(begin_tag);
    if (begin_pos == std::string::npos) return {content, false};
    auto end_pos = content.find(kEndMarker, begin_pos);
    if (end_pos == std::string::npos) return {content, false};
    end_pos += std::strlen(kEndMarker);
    // Eat trailing newline if present.
    if (end_pos < content.size() && content[end_pos] == '\n') ++end_pos;
    // Eat leading newline before the block too, to avoid leaving a
    // blank stub.
    auto strip_start = begin_pos;
    if (strip_start > 0 && content[strip_start - 1] == '\n' &&
        (strip_start == 1 || content[strip_start - 2] == '\n')) {
        --strip_start;
    }
    return {content.substr(0, strip_start) + content.substr(end_pos), true};
}

bool posix_add(const fs::path& dir) {
    auto paths = rc_paths();
    if (paths.empty()) return false;
    bool any_added = false;
    for (auto& rc : paths) {
        std::string content = read_file(rc);
        if (block_present(content, dir)) continue;  // already there
        if (!content.empty() && content.back() != '\n') content.push_back('\n');
        content.push_back('\n');  // blank line separator
        content += make_block(dir);
        if (write_file(rc, content)) any_added = true;
    }
    return any_added;
}

bool posix_remove(const fs::path& dir) {
    auto paths = rc_paths();
    if (paths.empty()) return false;
    bool any_removed = false;
    for (auto& rc : paths) {
        std::string content = read_file(rc);
        auto [stripped, did] = strip_block(content, dir);
        if (did) {
            if (write_file(rc, stripped)) any_removed = true;
        }
    }
    return any_removed;
}

}  // namespace
#endif  // !_WIN32

bool add_to_user_path(const fs::path& dir) {
#ifdef _WIN32
    std::wstring target = normalize(win::from_utf8(dir.string()));
    auto [current, type] = read_path_raw();
    auto parts = split_semi(current);

    for (const auto& p : parts) {
        if (ieq(normalize(p), target)) return false;
    }

    // 头插 — toolchain 优先于既有 entries（rustup 风格）。
    parts.insert(parts.begin(), target);

    // 类型策略：原本是哪种就保哪种。新建（无 PATH）默认 REG_EXPAND_SZ
    // 因为用户加的 entry 经常含 %USERPROFILE% 之类。
    if (type != REG_SZ && type != REG_EXPAND_SZ) type = REG_EXPAND_SZ;

    return write_path_raw(join_semi(parts), type);
#else
    return posix_add(dir);
#endif
}

bool remove_from_user_path(const fs::path& dir) {
#ifdef _WIN32
    std::wstring target = normalize(win::from_utf8(dir.string()));
    auto [current, type] = read_path_raw();
    auto parts = split_semi(current);

    auto orig = parts.size();
    parts.erase(std::remove_if(parts.begin(), parts.end(),
        [&](const std::wstring& p) { return ieq(normalize(p), target); }),
        parts.end());

    if (parts.size() == orig) return false;
    if (type != REG_SZ && type != REG_EXPAND_SZ) type = REG_EXPAND_SZ;
    return write_path_raw(join_semi(parts), type);
#else
    return posix_remove(dir);
#endif
}

std::vector<std::string> read_user_path() {
#ifdef _WIN32
    auto [current, _] = read_path_raw();
    std::vector<std::string> out;
    for (auto& w : split_semi(current)) out.push_back(win::to_utf8(w));
    return out;
#else
    return {};
#endif
}

bool set_user_env(const std::string& name, const std::string& value, bool expand) {
#ifdef _WIN32
    HKEY key = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, kEnvKey, 0, KEY_SET_VALUE, &key);
    if (rc != ERROR_SUCCESS) return false;

    std::wstring wname = win::from_utf8(name);
    std::wstring wval = win::from_utf8(value);
    DWORD type = expand ? REG_EXPAND_SZ : REG_SZ;
    DWORD size_bytes = static_cast<DWORD>((wval.size() + 1) * sizeof(wchar_t));
    rc = RegSetValueExW(key, wname.c_str(), 0, type,
                        reinterpret_cast<const BYTE*>(wval.data()), size_bytes);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) return false;

    DWORD_PTR result = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                        reinterpret_cast<LPARAM>(L"Environment"),
                        SMTO_ABORTIFHUNG, 5000, &result);
    return true;
#else
    (void)name; (void)value; (void)expand;
    return false;
#endif
}

bool unset_user_env(const std::string& name) {
#ifdef _WIN32
    HKEY key = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, kEnvKey, 0, KEY_SET_VALUE, &key);
    if (rc != ERROR_SUCCESS) return false;
    std::wstring wname = win::from_utf8(name);
    rc = RegDeleteValueW(key, wname.c_str());
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND) return false;
    DWORD_PTR result = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                        reinterpret_cast<LPARAM>(L"Environment"),
                        SMTO_ABORTIFHUNG, 5000, &result);
    return true;
#else
    (void)name;
    return false;
#endif
}

std::optional<std::string> get_user_env(const std::string& name) {
#ifdef _WIN32
    HKEY key = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, kEnvKey, 0, KEY_QUERY_VALUE, &key);
    if (rc != ERROR_SUCCESS) return std::nullopt;
    std::wstring wname = win::from_utf8(name);

    DWORD type = 0;
    DWORD size_bytes = 0;
    rc = RegQueryValueExW(key, wname.c_str(), nullptr, &type, nullptr, &size_bytes);
    if (rc != ERROR_SUCCESS) {
        RegCloseKey(key);
        return std::nullopt;
    }
    if (size_bytes == 0) {
        RegCloseKey(key);
        return std::string();
    }
    std::wstring buf(size_bytes / sizeof(wchar_t), L'\0');
    rc = RegQueryValueExW(key, wname.c_str(), nullptr, &type,
                          reinterpret_cast<LPBYTE>(buf.data()), &size_bytes);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) return std::nullopt;
    while (!buf.empty() && buf.back() == L'\0') buf.pop_back();
    return win::to_utf8(buf);
#else
    (void)name;
    return std::nullopt;
#endif
}

}  // namespace luban::win_path
