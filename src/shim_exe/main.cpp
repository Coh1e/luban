// luban-shim.exe — rustup-style native exe proxy.
//
// This is a separate executable (not part of luban.exe) that gets hard-linked
// into <data>/bin/<alias>.exe for every alias in installed.json. At runtime it:
//
//   1. GetModuleFileNameW → resolves its own absolute path (whichever name
//      the user invoked it as)
//   2. Strips dir + .exe → "alias"
//   3. Reads <self_dir>/.shim-table.json (a flat alias→absolute-path map
//      written by `luban shim`)
//   4. CreateProcessW to that target with the rest of argv preserved
//   5. Waits for child + propagates exit code
//
// Why this exists: `cmake.cmd` shims work for shells but cmake's compiler
// probe iterates PATH looking for `clang.exe` etc. (literally a .exe). With
// hard-linked native shims, we look like a real `clang.exe` and probes succeed.
//
// Design constraints:
//   - tiny binary (~200 KB static-linked); we don't link luban's own .cpp
//     modules so the shim stays compact
//   - no stdout chatter; errors go to stderr only on failure
//   - no allocations on the happy path beyond what nlohmann/json forces

#include <windows.h>
#include <shellapi.h>   // CommandLineToArgvW

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "json.hpp"

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring wide(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        w.data(), n);
    return w;
}

// Quote argv element per CommandLineToArgvW rules. Same as luban's proc.cpp.
std::wstring quote_arg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    int backslashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { ++backslashes; continue; }
        if (c == L'"') {
            out.append(static_cast<size_t>(backslashes) * 2 + 1, L'\\');
            backslashes = 0;
            out.push_back(L'"');
            continue;
        }
        out.append(static_cast<size_t>(backslashes), L'\\');
        backslashes = 0;
        out.push_back(c);
    }
    out.append(static_cast<size_t>(backslashes) * 2, L'\\');
    out.push_back(L'"');
    return out;
}

void die(const char* msg) {
    std::fprintf(stderr, "luban-shim: %s\n", msg);
    std::exit(127);
}

}  // namespace

// MinGW provides wmain via -municode link, but luban-shim isn't expected to
// face Unicode-named arguments often. Use the standard main() and convert via
// GetCommandLineW for consistency with luban.exe itself.
int main(int /*argc*/, char** /*argv*/) {
    // 1. self path
    wchar_t self_buf[MAX_PATH * 8];
    DWORD got = GetModuleFileNameW(nullptr, self_buf, static_cast<DWORD>(std::size(self_buf)));
    if (got == 0 || got >= std::size(self_buf)) die("GetModuleFileNameW failed");
    fs::path self(std::wstring(self_buf, got));

    // alias = filename stem (no .exe)
    std::wstring alias_w = self.stem().wstring();
    std::string alias = utf8(alias_w);

    // 2. find sibling .shim-table.json
    fs::path table_path = self.parent_path() / ".shim-table.json";
    std::ifstream in(table_path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr,
                     "luban-shim: could not open %s\n"
                     "  hint: run `luban shim` to (re)generate the shim table.\n",
                     utf8(table_path.wstring()).c_str());
        return 127;
    }
    json table;
    try {
        in >> table;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "luban-shim: parse %s: %s\n",
                     utf8(table_path.wstring()).c_str(), e.what());
        return 127;
    }
    if (!table.is_object() || !table.contains(alias)) {
        std::fprintf(stderr, "luban-shim: alias '%s' not in shim table\n", alias.c_str());
        return 127;
    }
    std::string target = table[alias].get<std::string>();
    std::wstring target_w = wide(target);

    // 3. preserve original commandline tail (everything after argv[0])
    LPWSTR raw_cmdline = GetCommandLineW();
    int n_args = 0;
    LPWSTR* argv_w = CommandLineToArgvW(raw_cmdline, &n_args);
    if (!argv_w) die("CommandLineToArgvW failed");

    std::wstring cmdline = quote_arg(target_w);
    for (int i = 1; i < n_args; ++i) {
        cmdline.push_back(L' ');
        cmdline += quote_arg(argv_w[i]);
    }
    LocalFree(argv_w);

    // 4. spawn target, inherit handles + cwd + env
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mut(cmdline.begin(), cmdline.end());
    mut.push_back(L'\0');

    if (!CreateProcessW(target_w.c_str(), mut.data(), nullptr, nullptr, TRUE,
                        0, nullptr, nullptr, &si, &pi)) {
        std::fprintf(stderr, "luban-shim: CreateProcessW failed for %s (err=%lu)\n",
                     target.c_str(), GetLastError());
        return 127;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}
