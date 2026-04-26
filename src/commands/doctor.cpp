// 1:1 port of luban_boot/commands/doctor.py.
// stdout output must match Python byte-for-byte modulo color escapes so we
// can dogfood-diff during the M1 transition.

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

#include "../cli.hpp"
#include "../log.hpp"
#include "../paths.hpp"
#include "../registry.hpp"

#ifdef _WIN32
#include <windows.h>
#include "../util/win.hpp"
#endif

namespace luban::commands {

namespace {

namespace fs = std::filesystem;

constexpr std::array<const char*, 9> kExpectedTools = {
    "clang", "clang++", "clangd", "clang-format", "clang-tidy",
    "cmake", "ninja", "git", "vcpkg",
};

// `which` — searches PATH, returns the first hit's absolute path.
std::string which(std::string_view tool) {
#ifdef _WIN32
    std::wstring wname = win::from_utf8(tool);
    // Per Python's shutil.which on Windows: try with no extension if name has
    // an extension, else iterate PATHEXT. SearchPathW handles PATHEXT for us.
    wchar_t buf[MAX_PATH * 4] = {};
    LPWSTR fp = nullptr;
    static const std::array<const wchar_t*, 5> exts = {L".exe", L".cmd", L".bat", L".com", L""};
    for (auto ext : exts) {
        std::wstring trial = wname;
        if (ext[0] != 0) trial += ext;
        DWORD got = SearchPathW(nullptr, trial.c_str(), nullptr,
                                static_cast<DWORD>(std::size(buf)), buf, &fp);
        if (got > 0 && got < std::size(buf)) {
            return win::to_utf8(std::wstring(buf, got));
        }
    }
    return {};
#else
    const char* path = std::getenv("PATH");
    if (!path) return {};
    std::string p(path);
    size_t start = 0;
    while (start <= p.size()) {
        size_t end = p.find(':', start);
        if (end == std::string::npos) end = p.size();
        fs::path candidate = fs::path(p.substr(start, end - start)) / std::string(tool);
        std::error_code ec;
        if (fs::exists(candidate, ec)) return candidate.string();
        if (end == p.size()) break;
        start = end + 1;
    }
    return {};
#endif
}

void println(const std::string& s) {
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fputc('\n', stdout);
}

std::string pad(std::string_view s, size_t width) {
    if (s.size() >= width) return std::string(s);
    std::string out(s);
    out.append(width - s.size(), ' ');
    return out;
}

std::string fmt_path(const fs::path& p) {
    // Use generic_string() would change separators; use the platform string.
    return p.string();
}

void print_homes() {
    log::step("Canonical homes");
    struct Row { std::string role; fs::path p; };
    std::array<Row, 4> rows = {{
        {"data",   paths::data_dir()},
        {"cache",  paths::cache_dir()},
        {"state",  paths::state_dir()},
        {"config", paths::config_dir()},
    }};
    size_t width = 0;
    for (auto& r : rows) width = std::max(width, r.role.size());
    std::error_code ec;
    for (auto& r : rows) {
        bool exists = fs::exists(r.p, ec);
        std::string marker = exists ? log::green("\xe2\x9c\x93") : log::dim("\xc2\xb7");
        println("  " + marker + " " + pad(r.role, width) + "  " + fmt_path(r.p));
    }
}

void print_volume_warning() {
    if (!paths::same_volume(paths::data_dir(), paths::cache_dir())) {
        log::warn("data and cache are on different volumes \xe2\x80\x94 hardlink "
                  "deduplication will fall back to copy.");
    }
}

void print_subdirs() {
    log::step("Sub-directories");
    auto all = paths::all_dirs();
    std::error_code ec;
    for (auto& [label, p] : all) {
        if (label == "data" || label == "cache" || label == "state" || label == "config") continue;
        bool exists = fs::exists(p, ec);
        std::string marker = exists ? log::green("\xe2\x9c\x93") : log::dim("\xc2\xb7");
        println("  " + marker + " " + pad(label, 18) + "  " + fmt_path(p));
    }
}

void print_installed() {
    log::step("Installed components");
    auto path = paths::installed_json_path();
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        println("  (none \xe2\x80\x94 run `luban-boot setup`)");
        return;
    }
    auto recs = registry::load_installed();
    if (recs.empty()) {
        println("  (registry empty \xe2\x80\x94 run `luban-boot setup`)");
        return;
    }
    for (auto& [name, rec] : recs) {
        std::string left = "  " + log::green("\xe2\x9c\x93") + " ";
        std::string n = pad(name, 28);
        std::string ver = rec.version.empty() ? std::string("?") : rec.version;
        println(left + n + " " + ver);
    }
}

void print_path_check() {
    log::step("Tools on PATH");
    for (auto* tool : kExpectedTools) {
        std::string found = which(tool);
        std::string marker = !found.empty() ? log::green("\xe2\x9c\x93") : log::dim("\xc2\xb7");
        std::string loc = !found.empty() ? found : log::dim("(not found)");
        println("  " + marker + " " + pad(tool, 14) + " " + loc);
    }
}

int run_doctor(const cli::ParsedArgs&) {
    print_homes();
    print_volume_warning();
    std::cout << '\n';
    print_subdirs();
    std::cout << '\n';
    print_installed();
    std::cout << '\n';
    print_path_check();
    return 0;
}

}  // namespace

void register_doctor() {
    cli::Subcommand c;
    c.name = "doctor";
    c.help = "report toolchain health";
    c.group = "advanced";
    c.long_help =
        "  Print luban's view of the world:\n"
        "    - the 4 canonical homes (data/cache/state/config)\n"
        "    - whether each subdirectory exists\n"
        "    - which components are recorded in installed.json\n"
        "    - whether the expected tools (clang/cmake/ninja/...) are on PATH";
    c.examples = {
        "luban doctor\tFull report",
    };
    c.run = run_doctor;
    cli::register_subcommand(std::move(c));
}

}  // namespace luban::commands
