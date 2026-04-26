#include "shim.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>

#include "paths.hpp"

namespace luban::shim {

namespace {

bool needs_cmd_quoting(const std::string& arg) {
    static constexpr std::string_view kSpecial = " \t\"&|<>";
    return arg.find_first_of(kSpecial) != std::string::npos;
}

std::string quote_cmd(const std::string& arg) {
    if (!needs_cmd_quoting(arg)) return arg;
    std::string out = "\"";
    for (char c : arg) {
        if (c == '"') out += "\\\""; else out.push_back(c);
    }
    out.push_back('"');
    return out;
}

std::string quote_ps1(const std::string& arg) {
    std::string out = "'";
    for (char c : arg) {
        if (c == '\'') out += "''"; else out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

std::string quote_sh(const std::string& arg) {
    std::string out = "\"";
    for (char c : arg) {
        if (c == '"') out += "\\\""; else out.push_back(c);
    }
    out.push_back('"');
    return out;
}

std::string join_prefix_cmd(const std::vector<std::string>& args) {
    if (args.empty()) return "";
    std::string out;
    for (auto& a : args) { out.push_back(' '); out += quote_cmd(a); }
    return out;
}

std::string join_prefix_ps1(const std::vector<std::string>& args) {
    if (args.empty()) return "";
    std::string out;
    for (auto& a : args) { out.push_back(' '); out += quote_ps1(a); }
    return out;
}

std::string join_prefix_sh(const std::vector<std::string>& args) {
    if (args.empty()) return "";
    std::string out;
    for (auto& a : args) { out.push_back(' '); out += quote_sh(a); }
    return out;
}

std::string to_posix(const fs::path& p) {
    std::string s = p.string();
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

void write_text(const fs::path& path, const std::string& content, bool unix_eol) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (unix_eol) out.write(content.data(), static_cast<std::streamsize>(content.size()));
    else          out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

}  // namespace

std::vector<fs::path> write_shim(const std::string& alias,
                                 const fs::path& exe,
                                 const std::vector<std::string>& prefix_args) {
    std::error_code ec;
    fs::create_directories(paths::bin_dir(), ec);

    fs::path bin = paths::bin_dir();
    fs::path cmd_path = bin / (alias + ".cmd");
    fs::path ps1_path = bin / (alias + ".ps1");
    fs::path sh_path  = bin / alias;

    std::string exe_str = exe.string();

    std::ostringstream cmd;
    cmd << "@echo off\r\n\""
        << exe_str << "\""
        << join_prefix_cmd(prefix_args)
        << " %*\r\n";
    write_text(cmd_path, cmd.str(), /*unix_eol=*/false);

    std::ostringstream ps1;
    ps1 << "$ErrorActionPreference = 'Continue'\r\n"
        << "& '" << exe_str << "'"
        << join_prefix_ps1(prefix_args)
        << " @args\r\n"
        << "exit $LASTEXITCODE\r\n";
    write_text(ps1_path, ps1.str(), /*unix_eol=*/false);

    std::ostringstream sh;
    sh << "#!/usr/bin/env bash\n"
       << "exec \"" << to_posix(exe) << "\""
       << join_prefix_sh(prefix_args)
       << " \"$@\"\n";
    write_text(sh_path, sh.str(), /*unix_eol=*/true);

#ifndef _WIN32
    fs::permissions(sh_path,
                    fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::add, ec);
#endif

    return {cmd_path, ps1_path, sh_path};
}

int remove_shim(const std::string& alias) {
    std::error_code ec;
    int n = 0;
    for (auto suffix : {".cmd", ".ps1", ""}) {
        fs::path p = paths::bin_dir() / (alias + suffix);
        if (fs::exists(p, ec) && fs::remove(p, ec)) ++n;
    }
    return n;
}

}  // namespace luban::shim
