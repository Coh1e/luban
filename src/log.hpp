#pragma once
// ANSI logger — writes to stderr (matches Python log.py behavior).
// VT mode is enabled lazily on first call when stderr is a TTY.

#include <format>
#include <string>
#include <string_view>

namespace luban::log {

void set_verbose(bool v);
bool is_verbose();

std::string dim(std::string_view s);
std::string bold(std::string_view s);
std::string red(std::string_view s);
std::string green(std::string_view s);
std::string yellow(std::string_view s);
std::string blue(std::string_view s);
std::string cyan(std::string_view s);

void info(std::string_view msg);
void step(std::string_view msg);
void ok(std::string_view msg);
void warn(std::string_view msg);
void err(std::string_view msg);
void debug(std::string_view msg);

template <class... Args>
void infof(std::format_string<Args...> fmt, Args&&... args) {
    info(std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void stepf(std::format_string<Args...> fmt, Args&&... args) {
    step(std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void okf(std::format_string<Args...> fmt, Args&&... args) {
    ok(std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void warnf(std::format_string<Args...> fmt, Args&&... args) {
    warn(std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void errf(std::format_string<Args...> fmt, Args&&... args) {
    err(std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace luban::log
