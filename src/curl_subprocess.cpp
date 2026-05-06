// See `curl_subprocess.hpp` for design rationale.

#include "curl_subprocess.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include <windows.h>

#include "log.hpp"
#include "luban/version.hpp"
#include "util/win.hpp"

namespace luban::curl_subprocess {

namespace {

namespace fs = std::filesystem;

// ---- curl.exe location ----------------------------------------------

fs::path locate_curl_once() {
    // 1. <System32>\curl.exe — guaranteed present on Win10 1803+ (May 2018);
    //    the Windows base image policy hasn't dropped it. SearchPath would
    //    also find this, but we hit the explicit path first to avoid a PATH
    //    that's been reordered to put a different curl ahead.
    wchar_t sys[MAX_PATH];
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        fs::path p = fs::path(sys) / L"curl.exe";
        std::error_code ec;
        if (fs::is_regular_file(p, ec)) return p;
    }

    // 2. SearchPath fallback — covers Server SKUs / unusual installs / users
    //    who stripped System32 curl in some lockdown image.
    wchar_t buf[MAX_PATH];
    DWORD len = SearchPathW(nullptr, L"curl.exe", nullptr, MAX_PATH, buf, nullptr);
    if (len > 0 && len < MAX_PATH) return fs::path(buf);

    return {};
}

fs::path& cached_curl() {
    static fs::path p = locate_curl_once();
    return p;
}

// ---- command-line construction --------------------------------------

// User-Agent: matches what download.cpp's WinHTTP path used to send.
// kLubanVersion is std::string_view → wrap in std::string() before op+
// since std::string + std::string_view isn't operator-overloaded pre-C++26.
std::wstring user_agent() {
    std::string narrow = std::string("luban/") +
                         std::string(luban::kLubanVersion) +
                         std::string(" (+https://github.com/Coh1e/luban)");
    return win::from_utf8(narrow);
}

// Quote one argv item per CommandLineToArgvW rules. Backslashes only need
// special handling immediately before a quote; otherwise pass through.
// Always wrap in quotes so spaces / special chars don't fragment the token.
std::wstring quote(std::wstring_view s) {
    std::wstring out;
    out.reserve(s.size() + 2);
    out.push_back(L'"');
    int pending_backslashes = 0;
    for (wchar_t c : s) {
        if (c == L'\\') {
            ++pending_backslashes;
            continue;
        }
        if (c == L'"') {
            // Escape pending backslashes (so they remain backslashes) AND the quote.
            for (int i = 0; i < pending_backslashes * 2; ++i) out.push_back(L'\\');
            pending_backslashes = 0;
            out.push_back(L'\\');
            out.push_back(L'"');
            continue;
        }
        for (int i = 0; i < pending_backslashes; ++i) out.push_back(L'\\');
        pending_backslashes = 0;
        out.push_back(c);
    }
    // Trailing backslashes need doubling because the closing quote follows.
    for (int i = 0; i < pending_backslashes * 2; ++i) out.push_back(L'\\');
    out.push_back(L'"');
    return out;
}

std::wstring build_cmdline(const fs::path& curl_exe,
                            const std::string& url,
                            const fs::path& dest,
                            const Options& opts) {
    std::wstring cmd = quote(curl_exe.wstring());

    auto add = [&](std::wstring_view s) {
        cmd.push_back(L' ');
        cmd.append(s);
    };
    auto add_q = [&](std::wstring_view s) {
        cmd.push_back(L' ');
        cmd.append(quote(s));
    };
    auto add_q_str = [&](std::string_view s) { add_q(win::from_utf8(std::string(s))); };

    // -fSL: fail (HTTP non-2xx → exit 22), follow redirects, show errors
    // -sS: silent + show errors (no curl progress meter — we render our own)
    add(L"-fsSL");
    if (opts.resume) add(L"-C -");
    add(L"--connect-timeout"); add(std::to_wstring(opts.connect_timeout_seconds));
    add(L"--max-time");        add(std::to_wstring(opts.max_time_seconds));
    add(L"-A"); add_q(user_agent());
    add(L"-o"); add_q(dest.wstring());
    add_q_str(url);
    return cmd;
}

// ---- subprocess launch + progress polling ---------------------------

struct ProcResult {
    DWORD exit_code = 0;
    std::string stderr_text;
    bool stalled = false;        ///< True if killed by stall watchdog
    bool spawn_failed = false;   ///< CreateProcess failed
    std::string spawn_error;
};

// Drive an in-process polling thread that:
//   1. reads `dest` size every ~100 ms
//   2. feeds the delta into `prog->update()`
//   3. tracks 15s rolling window for stall detection
//   4. terminates the process if stalled — caller's retry loop picks up
//
// Lifetime: thread spawned before WaitForSingleObject; signals shutdown
// via atomic flag once the parent function completes Wait. Joins before
// returning. Atomic flag also lets the thread bail early if the process
// exits (poll WaitForSingleObject(0) each tick).
ProcResult run_curl(const fs::path& curl_exe,
                    const std::wstring& cmdline_w,
                    const fs::path& dest,
                    const Options& opts) {
    ProcResult r;

    // Capture curl's stderr to a tmp file so we can surface its diagnostic
    // text on failure. Using SECURITY_ATTRIBUTES with bInheritHandle=true
    // and CreateProcessW with bInheritHandles=true.
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    wchar_t tmp_dir[MAX_PATH];
    if (GetTempPathW(MAX_PATH, tmp_dir) == 0) {
        wcscpy_s(tmp_dir, L".\\");
    }
    fs::path stderr_path = fs::path(tmp_dir) /
        (L"luban-curl-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
         std::to_wstring(::GetTickCount64()) + L".err");
    HANDLE h_err = CreateFileW(stderr_path.wstring().c_str(),
                               GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h_err == INVALID_HANDLE_VALUE) {
        r.spawn_failed = true;
        r.spawn_error = "open stderr capture file failed";
        return r;
    }

    // Pipe stdin + stdout to /dev/null-equivalent so curl doesn't inherit
    // PowerShell's console (would print stray progress otherwise; we use
    // -sS but defensive).
    HANDLE h_null = CreateFileW(L"NUL",
                                GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h_null == INVALID_HANDLE_VALUE) {
        CloseHandle(h_err);
        std::error_code ec; fs::remove(stderr_path, ec);
        r.spawn_failed = true;
        r.spawn_error = "open NUL failed";
        return r;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = h_null;
    si.hStdOutput = h_null;
    si.hStdError  = h_err;

    PROCESS_INFORMATION pi{};
    std::wstring cmdline_mut = cmdline_w;  // CreateProcessW may modify

    BOOL ok = CreateProcessW(curl_exe.wstring().c_str(),
                              cmdline_mut.data(),
                              nullptr, nullptr,
                              TRUE,  // bInheritHandles — h_err / h_null
                              CREATE_NO_WINDOW,
                              nullptr, nullptr,
                              &si, &pi);
    CloseHandle(h_null);
    if (!ok) {
        CloseHandle(h_err);
        std::error_code ec; fs::remove(stderr_path, ec);
        r.spawn_failed = true;
        r.spawn_error = "CreateProcessW failed (GetLastError=" +
                        std::to_string(::GetLastError()) + ")";
        return r;
    }
    CloseHandle(h_err);  // Child holds its own handle now

    // Polling thread: drive progress + stall detection.
    std::atomic<bool> killed_for_stall{false};
    std::atomic<bool> shutdown{false};
    std::int64_t initial_size = 0;
    {
        std::error_code ec;
        if (fs::exists(dest, ec)) initial_size = static_cast<std::int64_t>(fs::file_size(dest, ec));
        if (ec) initial_size = 0;
    }

    std::thread watcher([&]() {
        using clock = std::chrono::steady_clock;
        auto t0 = clock::now();
        auto stall_window_start = t0;
        std::int64_t stall_window_start_bytes = initial_size;
        std::int64_t last_reported_size = initial_size;

        while (!shutdown.load(std::memory_order_relaxed)) {
            // Sleep first so curl gets a moment to start writing.
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            // Did the process exit?
            DWORD wr = WaitForSingleObject(pi.hProcess, 0);
            if (wr == WAIT_OBJECT_0) break;

            // Poll dest size.
            std::error_code ec;
            std::int64_t cur = 0;
            if (fs::exists(dest, ec)) {
                cur = static_cast<std::int64_t>(fs::file_size(dest, ec));
                if (ec) cur = last_reported_size;
            }

            // Feed progress delta (avoid going backwards on transient FS race).
            if (opts.progress && cur > last_reported_size) {
                opts.progress->update(static_cast<std::size_t>(cur - last_reported_size));
                last_reported_size = cur;
            }

            // Stall detection: if no growth in kStallWindowSeconds *after*
            // an initial warmup, kill curl. Warmup = same window width;
            // first window is "starting up, give it a chance".
            auto now = clock::now();
            auto window_dt = std::chrono::duration<double>(now - stall_window_start).count();
            if (window_dt >= static_cast<double>(kStallWindowSeconds)) {
                std::int64_t window_bytes = cur - stall_window_start_bytes;
                double rate = static_cast<double>(window_bytes) / window_dt;
                bool warmup_done = std::chrono::duration<double>(now - t0).count() >=
                                   static_cast<double>(kStallWindowSeconds);
                if (warmup_done && rate < static_cast<double>(kStallMinBytesPerSec)) {
                    killed_for_stall.store(true, std::memory_order_relaxed);
                    TerminateProcess(pi.hProcess, 1);
                    return;
                }
                stall_window_start = now;
                stall_window_start_bytes = cur;
            }
        }
    });

    WaitForSingleObject(pi.hProcess, INFINITE);
    shutdown.store(true, std::memory_order_relaxed);
    if (watcher.joinable()) watcher.join();

    // Final size + delta sweep — covers the case where the file finished
    // growing in the last 100ms before exit.
    if (opts.progress) {
        std::error_code ec;
        std::int64_t final_sz = static_cast<std::int64_t>(fs::file_size(dest, ec));
        if (!ec) {
            std::int64_t delta = final_sz - initial_size - opts.progress->bytes_done();
            if (delta > 0) {
                opts.progress->update(static_cast<std::size_t>(delta));
            }
        }
    }

    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    r.exit_code = code;
    r.stalled = killed_for_stall.load(std::memory_order_relaxed);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    // Slurp captured stderr.
    {
        std::ifstream in(stderr_path, std::ios::binary);
        std::ostringstream ss;
        if (in) ss << in.rdbuf();
        r.stderr_text = ss.str();
        // Trim trailing whitespace.
        while (!r.stderr_text.empty() &&
               (r.stderr_text.back() == '\n' || r.stderr_text.back() == '\r' ||
                r.stderr_text.back() == ' '  || r.stderr_text.back() == '\t')) {
            r.stderr_text.pop_back();
        }
    }
    std::error_code ec; fs::remove(stderr_path, ec);
    return r;
}

// curl exit codes → ErrorKind. https://curl.se/libcurl/c/libcurl-errors.html
ErrorKind classify(int curl_code, const std::string& stderr_text) {
    switch (curl_code) {
        case 22: {
            // -f makes curl emit "The requested URL returned error: <code>".
            // Parse the status to distinguish 4xx (don't retry) from 5xx
            // (retry). If we can't parse, default to client (more cautious).
            auto pos = stderr_text.find("error: ");
            if (pos != std::string::npos) {
                int status = std::atoi(stderr_text.c_str() + pos + 7);
                if (status >= 500) return ErrorKind::HttpServer;
            }
            return ErrorKind::HttpClient;
        }
        // Network errors that retry can fix (transient or recoverable).
        case 6:   // resolve host
        case 7:   // connect
        case 28:  // timeout
        case 35:  // TLS handshake
        case 52:  // empty reply
        case 56:  // recv failure (TCP RST mid-transfer — VN's friend)
        case 92:  // h2 stream error
            return ErrorKind::Network;
        default:
            return ErrorKind::Network;
    }
}

}  // namespace

fs::path find_curl_exe() { return cached_curl(); }

std::expected<Result, Error> download_to_file(
    const std::string& url, const fs::path& dest, const Options& opts) {
    fs::path curl = cached_curl();
    if (curl.empty()) {
        return std::unexpected(Error{ErrorKind::NotFound,
            "curl.exe not found in System32 or PATH (Win10 1803+ baseline)", 0});
    }

    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);

    std::int64_t before = 0;
    if (fs::exists(dest, ec)) before = static_cast<std::int64_t>(fs::file_size(dest, ec));

    auto cmdline = build_cmdline(curl, url, dest, opts);
    auto p = run_curl(curl, cmdline, dest, opts);

    if (p.spawn_failed) {
        return std::unexpected(Error{ErrorKind::SpawnFailed, p.spawn_error, 0});
    }
    if (p.stalled) {
        std::error_code ec_rm; fs::remove(dest, ec_rm);  // tainted partial
        return std::unexpected(Error{ErrorKind::Stalled,
            "no progress for " + std::to_string(kStallWindowSeconds) + "s", 0});
    }
    if (p.exit_code != 0) {
        std::int64_t after = 0;
        if (fs::exists(dest, ec)) after = static_cast<std::int64_t>(fs::file_size(dest, ec));
        // Don't delete on Network errors — partial bytes let -C - resume.
        // DO delete on HttpClient (4xx) — caller bails, no point keeping.
        ErrorKind k = classify(static_cast<int>(p.exit_code), p.stderr_text);
        if (k == ErrorKind::HttpClient || k == ErrorKind::HttpServer) {
            std::error_code ec_rm; fs::remove(dest, ec_rm);
        }
        std::string msg = "curl exit " + std::to_string(p.exit_code);
        if (!p.stderr_text.empty()) {
            msg += ": " + p.stderr_text;
        }
        if (k == ErrorKind::Network && after > before) {
            msg += " (resume from " + std::to_string(after) + " B on retry)";
        }
        return std::unexpected(Error{k, std::move(msg),
                                     static_cast<int>(p.exit_code)});
    }

    std::int64_t after = static_cast<std::int64_t>(fs::file_size(dest, ec));
    if (ec) {
        return std::unexpected(Error{ErrorKind::Io, "cannot stat dest", 0});
    }
    return Result{after - before, after};
}

std::optional<std::int64_t> head_content_length(
    const std::string& url, int timeout_seconds) {
    fs::path curl = cached_curl();
    if (curl.empty()) return std::nullopt;

    // curl -sSL -I -A <ua> --connect-timeout T --max-time T <url>
    // Output is the response headers (one block per redirect hop because of
    // -L). We scan for the LAST Content-Length: line. Case-insensitive header
    // names per RFC 7230, but curl normalizes to "Content-Length" in practice.
    wchar_t tmp_dir[MAX_PATH];
    if (GetTempPathW(MAX_PATH, tmp_dir) == 0) wcscpy_s(tmp_dir, L".\\");
    fs::path out_tmp = fs::path(tmp_dir) /
        (L"luban-head-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
         std::to_wstring(::GetTickCount64()) + L".hdr");

    std::wstring cmd = quote(curl.wstring());
    auto add = [&](std::wstring_view s) { cmd.push_back(L' '); cmd.append(s); };
    auto add_q = [&](std::wstring_view s) { cmd.push_back(L' '); cmd.append(quote(s)); };
    add(L"-sSL -I");
    add(L"-A"); add_q(user_agent());
    add(L"--connect-timeout"); add(std::to_wstring(std::min(timeout_seconds, 30)));
    add(L"--max-time");        add(std::to_wstring(timeout_seconds));
    add(L"-o"); add_q(out_tmp.wstring());
    add_q(win::from_utf8(url));

    SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    HANDLE h_null = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = h_null; si.hStdOutput = h_null; si.hStdError = h_null;
    PROCESS_INFORMATION pi{};
    std::wstring cmd_mut = cmd;
    BOOL ok = CreateProcessW(curl.wstring().c_str(), cmd_mut.data(),
                              nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                              nullptr, nullptr, &si, &pi);
    if (h_null != INVALID_HANDLE_VALUE) CloseHandle(h_null);
    if (!ok) {
        std::error_code ec; fs::remove(out_tmp, ec);
        return std::nullopt;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    if (code != 0) {
        std::error_code ec; fs::remove(out_tmp, ec);
        return std::nullopt;
    }

    std::ifstream in(out_tmp, std::ios::binary);
    std::ostringstream ss; if (in) ss << in.rdbuf();
    std::string headers = ss.str();
    std::error_code ec; fs::remove(out_tmp, ec);

    // Walk lines; remember the last Content-Length we see. -L follows
    // redirects, so the last block is the final response.
    std::optional<std::int64_t> last;
    std::size_t pos = 0;
    while (pos < headers.size()) {
        std::size_t eol = headers.find('\n', pos);
        std::string_view line(headers.data() + pos,
                              (eol == std::string::npos) ? headers.size() - pos : eol - pos);
        // case-insensitive "content-length:" match
        constexpr std::string_view tag = "content-length:";
        if (line.size() > tag.size()) {
            bool match = true;
            for (std::size_t i = 0; i < tag.size(); ++i) {
                char a = static_cast<char>(std::tolower(static_cast<unsigned char>(line[i])));
                if (a != tag[i]) { match = false; break; }
            }
            if (match) {
                auto v = line.substr(tag.size());
                while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
                while (!v.empty() && (v.back() == '\r' || v.back() == ' ' || v.back() == '\t'))
                    v.remove_suffix(1);
                try {
                    last = std::stoll(std::string(v));
                } catch (...) { /* skip malformed */ }
            }
        }
        if (eol == std::string::npos) break;
        pos = eol + 1;
    }
    return last;
}

std::expected<std::string, Error> fetch_text(
    const std::string& url, int timeout_seconds) {
    fs::path curl = cached_curl();
    if (curl.empty()) {
        return std::unexpected(Error{ErrorKind::NotFound,
            "curl.exe not found", 0});
    }

    wchar_t tmp_dir[MAX_PATH];
    if (GetTempPathW(MAX_PATH, tmp_dir) == 0) wcscpy_s(tmp_dir, L".\\");
    fs::path tmp = fs::path(tmp_dir) /
        (L"luban-fetch-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
         std::to_wstring(::GetTickCount64()) + L".tmp");

    Options opts;
    opts.connect_timeout_seconds = std::min(timeout_seconds, 30);
    opts.max_time_seconds = timeout_seconds;
    opts.resume = false;  // body fetched fresh each call; no prior partial
    opts.progress = nullptr;

    auto rc = download_to_file(url, tmp, opts);
    if (!rc) {
        std::error_code ec; fs::remove(tmp, ec);
        return std::unexpected(rc.error());
    }

    // Slurp file into string.
    std::ifstream in(tmp, std::ios::binary);
    std::ostringstream ss;
    if (in) ss << in.rdbuf();
    std::string body = ss.str();
    std::error_code ec; fs::remove(tmp, ec);
    return body;
}

}  // namespace luban::curl_subprocess
