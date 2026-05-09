#include "log.hpp"

#include <atomic>
#include <cstdio>
#include <format>
#include <iostream>
#include <optional>

#include <windows.h>
#include <io.h>

namespace luban::log {

namespace {

std::atomic<bool> g_verbose{false};

bool stderr_is_tty() {
    return _isatty(_fileno(stderr));
}

bool no_color_env() {
    wchar_t buf[2];
    DWORD n = GetEnvironmentVariableW(L"NO_COLOR", buf, 2);
    return n > 0;
}

bool enable_vt_on_windows() {
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD mode = 0;
    if (!GetConsoleMode(h, &mode)) return false;
    return SetConsoleMode(h, mode | 0x0004 /*ENABLE_VIRTUAL_TERMINAL_PROCESSING*/) != 0;
}

bool ansi_enabled() {
    static std::optional<bool> cached;
    if (cached) return *cached;
    if (no_color_env()) cached = false;
    else if (!stderr_is_tty()) cached = false;
    else cached = enable_vt_on_windows();
    return *cached;
}

std::string wrap(const char* code, std::string_view text) {
    if (!ansi_enabled()) return std::string(text);
    return std::format("\x1b[{}m{}\x1b[0m", code, text);
}

void emit(const std::string& prefix, std::string_view msg) {
    std::fprintf(stderr, "%s %.*s\n",
                 prefix.c_str(), static_cast<int>(msg.size()), msg.data());
    std::fflush(stderr);
}

}  // namespace

void set_verbose(bool v) { g_verbose.store(v); }
bool is_verbose() { return g_verbose.load(); }

std::string dim(std::string_view s)    { return wrap("2",  s); }
std::string bold(std::string_view s)   { return wrap("1",  s); }
std::string red(std::string_view s)    { return wrap("31", s); }
std::string green(std::string_view s)  { return wrap("32", s); }
std::string yellow(std::string_view s) { return wrap("33", s); }
std::string blue(std::string_view s)   { return wrap("34", s); }
std::string cyan(std::string_view s)   { return wrap("36", s); }

void info(std::string_view msg)  { emit(blue("\xc2\xb7"), msg); }                // "·"
void step(std::string_view msg)  { emit(cyan("\xe2\x86\x92"), bold(msg)); }      // "→"
void ok(std::string_view msg)    { emit(green("\xe2\x9c\x93"), msg); }           // "✓"
void warn(std::string_view msg)  { emit(yellow("!"), yellow(msg)); }
void err(std::string_view msg)   { emit(red("\xe2\x9c\x97"), red(msg)); }        // "✗"
void debug(std::string_view msg) {
    if (g_verbose.load()) emit(dim("\xe2\x80\xa6"), dim(msg));                   // "…"
}

}  // namespace luban::log
