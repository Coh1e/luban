#include "msvc_env.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <system_error>

#include <windows.h>
#include "util/win.hpp"

#include "file_util.hpp"
#include "log.hpp"
#include "paths.hpp"

namespace luban::msvc_env {

namespace {

using nlohmann::json;

// Variables vcvarsall sets that are noisy / transient / not needed to
// reproduce the build environment in a child process. We strip these from
// the diff to keep the persisted file readable and to avoid confusing the
// child shell with vcvars's internal bookkeeping.
const std::set<std::string, std::less<>>& transient_vars() {
    static const std::set<std::string, std::less<>> s = {
        "PROMPT",                       // vcvarsall changes the prompt
        "__VSCMD_PREINIT_PATH",         // bookkeeping for the script itself
        "__VSCMD_script_err_count",
        "VSCMD_DEBUG",
        "VS170COMNTOOLS",               // version-specific aliases — not portable
    };
    return s;
}

// Run cmd.exe /c "<command>" and capture stdout. Caller is responsible for
// quoting `command` (which is appended after `/c ` literally, so embedded
// quotes need to be balanced). On failure returns empty string.
std::string run_capture_cmd(const std::string& command) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return {};
    if (!SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(rd); CloseHandle(wr);
        return {};
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError  = wr;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::wstring full = L"cmd.exe /d /c " + win::from_utf8(command);
    std::vector<wchar_t> mut(full.begin(), full.end());
    mut.push_back(L'\0');

    if (!CreateProcessW(nullptr, mut.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr);
        return {};
    }
    CloseHandle(wr);  // close write end so ReadFile sees EOF when child exits

    std::string out;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0) {
        out.append(buf, buf + n);
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return out;
}

// Parse the output of `set` (one VAR=VALUE per line) into a map. Skips
// blank lines and lines without `=`. Trailing CR/LF stripped per-line.
std::map<std::string, std::string> parse_set_output(const std::string& text) {
    std::map<std::string, std::string> out;
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        // Strip trailing \r (Windows cmd uses CRLF).
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        if (line.empty()) continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (key.empty()) continue;
        out[std::move(key)] = std::move(val);
    }
    return out;
}

// Diff two PATH strings (semicolon-separated). Returns the leading
// segment of `after` that isn't present anywhere in `before` — this is
// what vcvarsall prepended. We assume vcvarsall ALWAYS prepends (it does
// in practice; never appends or rewrites in-place).
std::string path_prefix_diff(const std::string& before, const std::string& after) {
    if (after.size() <= before.size()) return {};
    // vcvarsall's pattern: "<msvc dirs>;<original PATH>". So if `after`
    // ends with `;<before>`, the prefix is `after.substr(0, after.size() - before.size() - 1)`.
    if (after.size() >= before.size() + 1
        && after.compare(after.size() - before.size(), before.size(), before) == 0
        && after[after.size() - before.size() - 1] == ';') {
        return after.substr(0, after.size() - before.size() - 1);
    }
    // Fallback: no clean suffix match — return the full `after`. Caller will
    // prepend the lot, accepting some PATH duplication; safer than dropping
    // the MSVC dirs entirely.
    return after;
}

// ISO-8601 timestamp (UTC, second precision). Used for the captured_at
// metadata field — surfaces in `describe --host` so users can tell when
// the capture happened (and re-capture if VS got upgraded since).
std::string iso_now() {
    using namespace std::chrono;
    auto t = system_clock::to_time_t(system_clock::now());
    std::tm gm{};
    gmtime_s(&gm, &t);
    std::ostringstream ss;
    ss << std::put_time(&gm, "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

}  // namespace

fs::path file_path() { return paths::state_dir() / "msvc-env.json"; }

fs::path find_vswhere() {
    // Microsoft-documented well-known path. vswhere ships with every VS
    // install (>=2017) at this exact location.
    auto pf86 = paths::from_env("ProgramFiles(x86)");
    if (!pf86) return {};
    fs::path p = *pf86 / "Microsoft Visual Studio" / "Installer" / "vswhere.exe";
    std::error_code ec;
    return fs::exists(p, ec) ? p : fs::path{};
}

fs::path find_install() {
    fs::path vswhere = find_vswhere();
    if (vswhere.empty()) return {};

    // -latest         pick newest install if multiple
    // -property installationPath
    // -products *     accept Build Tools too (not just full VS)
    std::string cmd = "\"" + vswhere.string() + "\" -latest -products * "
                      "-requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 "
                      "-property installationPath";
    std::string out = run_capture_cmd(cmd);
    while (!out.empty() && (out.back() == '\r' || out.back() == '\n' || out.back() == ' ')) {
        out.pop_back();
    }
    if (out.empty()) return {};
    return fs::path(out);
}

fs::path find_vcvarsall(const fs::path& install_path) {
    if (install_path.empty()) return {};
    fs::path p = install_path / "VC" / "Auxiliary" / "Build" / "vcvarsall.bat";
    std::error_code ec;
    return fs::exists(p, ec) ? p : fs::path{};
}

std::optional<Captured> capture(const fs::path& install_path, const std::string& arch) {
    fs::path vcvars = find_vcvarsall(install_path);
    if (vcvars.empty()) {
        log::errf("vcvarsall.bat not found under {}", install_path.string());
        return std::nullopt;
    }

    // Pass 1: baseline env. We use cmd's `set` builtin so we get exactly
    // what cmd thinks is in the env (case-insensitive var names, etc.).
    auto before = parse_set_output(run_capture_cmd("set"));

    // Pass 2: env after vcvarsall. Quoting: the whole command has to be
    // wrapped in double quotes for cmd /c, and inner quotes around paths
    // with spaces. cmd's parser handles two leading quotes as "the outer
    // pair" and lets the rest pass through.
    std::string p2 = "\"\"" + vcvars.string() + "\" " + arch + " >nul && set\"";
    auto after = parse_set_output(run_capture_cmd(p2));

    if (after.empty()) {
        log::err("vcvarsall produced no output — capture failed");
        return std::nullopt;
    }

    Captured c;
    c.vs_install_path = install_path.string();
    c.arch = arch;
    c.captured_at = iso_now();

    // Diff: keys in `after` that are new or have different values than
    // `before`, minus the transient bookkeeping vars.
    auto& skip = transient_vars();
    for (auto& [k, v] : after) {
        if (skip.count(k)) continue;
        if (k == "PATH" || k == "Path") continue;  // handled separately
        auto it = before.find(k);
        if (it == before.end() || it->second != v) {
            c.vars[k] = v;
        }
    }

    // PATH delta: extract just the leading segment vcvarsall prepended.
    std::string before_path, after_path;
    auto find_path_value = [](const std::map<std::string, std::string>& m) -> std::string {
        for (auto& [k, v] : m) {
            // cmd's `set` uses the case-preserving form (usually "Path");
            // accept both for portability.
            if (k == "PATH" || k == "Path") return v;
        }
        return {};
    };
    before_path = find_path_value(before);
    after_path  = find_path_value(after);
    c.path_addition = path_prefix_diff(before_path, after_path);

    return c;
}

bool save(const Captured& c) {
    json doc = json::object();
    doc["schema"] = 1;
    doc["vs_install_path"] = c.vs_install_path;
    doc["arch"] = c.arch;
    doc["captured_at"] = c.captured_at;
    doc["path_addition"] = c.path_addition;
    json kv = json::object();
    for (auto& [k, v] : c.vars) kv[k] = v;
    doc["vars"] = kv;

    fs::path target = file_path();
    std::string text = doc.dump(2);
    text.push_back('\n');  // trailing newline matches prior behaviour
    return file_util::write_text_atomic(target, text);
}

std::optional<Captured> load() {
    fs::path p = file_path();
    std::error_code ec;
    if (!fs::exists(p, ec)) return std::nullopt;
    std::string text = file_util::read_text_no_bom(p);
    if (text.empty()) return std::nullopt;
    json doc;
    try { doc = json::parse(text); } catch (...) { return std::nullopt; }
    if (!doc.is_object()) return std::nullopt;

    Captured c;
    c.vs_install_path = doc.value("vs_install_path", "");
    c.arch = doc.value("arch", "");
    c.captured_at = doc.value("captured_at", "");
    c.path_addition = doc.value("path_addition", "");
    if (doc.contains("vars") && doc["vars"].is_object()) {
        for (auto& [k, v] : doc["vars"].items()) {
            if (v.is_string()) c.vars[k] = v.get<std::string>();
        }
    }
    return c;
}

void clear() {
    std::error_code ec;
    fs::remove(file_path(), ec);
}

}  // namespace luban::msvc_env
