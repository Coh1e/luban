// Windows-only subprocess runner. v1.0.5+ dropped the POSIX fork/exec
// fallback (DESIGN §1 Windows-first; luban targets fresh Win11 boxes,
// not Linux/macOS).

#include "proc.hpp"

#include <cstdlib>
#include <sstream>

#include <windows.h>
#include "util/win.hpp"

namespace luban::proc {

namespace {

// Quote a single argv element per CommandLineToArgvW rules. Sufficient for
// our usage (cmake/clang invocations).
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

std::wstring build_cmdline(const std::vector<std::string>& cmd) {
    std::wstring line;
    for (size_t i = 0; i < cmd.size(); ++i) {
        if (i) line.push_back(L' ');
        line += quote_arg(win::from_utf8(cmd[i]));
    }
    return line;
}

// Build a WCHAR env block: KEY=VAL\0KEY=VAL\0\0.
// Starts from current process env, then overlays overrides (case-insensitive
// on Windows — we lowercase the lookup key).
std::wstring build_env_block(const std::map<std::string, std::string>& overrides) {
    std::map<std::wstring, std::wstring> merged;

    LPWCH parent = GetEnvironmentStringsW();
    if (parent) {
        for (LPWCH p = parent; *p; ) {
            std::wstring entry(p);
            p += entry.size() + 1;
            auto eq = entry.find(L'=');
            if (eq == std::wstring::npos || eq == 0) continue;
            std::wstring key = entry.substr(0, eq);
            std::wstring val = entry.substr(eq + 1);
            // Case-insensitive merge: lowercase key for the map.
            std::wstring k = key;
            for (auto& ch : k) ch = static_cast<wchar_t>(towlower(ch));
            merged[k] = key + L'=' + val;
        }
        FreeEnvironmentStringsW(parent);
    }
    for (auto& [k, v] : overrides) {
        std::wstring wk = win::from_utf8(k);
        std::wstring wv = win::from_utf8(v);
        std::wstring lk = wk;
        for (auto& ch : lk) ch = static_cast<wchar_t>(towlower(ch));
        merged[lk] = wk + L'=' + wv;
    }

    std::wstring block;
    for (auto& [_, entry] : merged) {
        block += entry;
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

}  // namespace

int run(const std::vector<std::string>& cmd,
        const std::string& cwd,
        const std::map<std::string, std::string>& env_overrides) {
    if (cmd.empty()) return -1;

    std::wstring cmdline = build_cmdline(cmd);
    std::wstring env_block = build_env_block(env_overrides);
    std::wstring wcwd = win::from_utf8(cwd);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    // CreateProcessW mutates the cmdline buffer; give it a writable copy.
    std::vector<wchar_t> mutable_cmdline(cmdline.begin(), cmdline.end());
    mutable_cmdline.push_back(L'\0');

    BOOL ok = CreateProcessW(
        /*lpApplicationName=*/nullptr,
        /*lpCommandLine=*/mutable_cmdline.data(),
        /*lpProcessAttributes=*/nullptr,
        /*lpThreadAttributes=*/nullptr,
        /*bInheritHandles=*/TRUE,
        /*dwCreationFlags=*/CREATE_UNICODE_ENVIRONMENT,
        /*lpEnvironment=*/env_block.data(),
        /*lpCurrentDirectory=*/wcwd.empty() ? nullptr : wcwd.c_str(),
        &si, &pi);

    if (!ok) return -1;

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

}  // namespace luban::proc
