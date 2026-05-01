#include "shim.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>

#include "json.hpp"

#include "file_util.hpp"
#include "paths.hpp"

namespace luban::shim {

namespace {

using nlohmann::json;

// Path of the alias→exe map shipped alongside shims. The luban-shim.exe binary
// also reads this file (sibling lookup) at runtime to translate its hardlinked
// alias back into a target exe path.
fs::path table_path() { return paths::bin_dir() / ".shim-table.json"; }

// Quoting helpers for the .cmd shim. We don't generate .ps1 / .sh anymore.
bool needs_cmd_quoting(const std::string& arg) {
    // Conservative whitelist: any of these triggers double-quote wrapping.
    static constexpr std::string_view kSpecial = " \t\"&|<>";
    return arg.find_first_of(kSpecial) != std::string::npos;
}

std::string quote_cmd(const std::string& arg) {
    if (!needs_cmd_quoting(arg)) return arg;
    // CMD has no real escape mechanism; backslash-escaping inner quotes is the
    // best we can do (matches most user expectations).
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

void write_text_crlf(const fs::path& path, const std::string& content) {
    // CRLF — .cmd parsers tolerate LF but produce subtle hangs on some
    // versions of cmd.exe with embedded LF. CRLF is the safe choice.
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

// Read the shim table; returns empty map if file is missing or unparseable.
// Must NOT throw — called from collision-detection paths during install.
std::map<std::string, std::string> load_table() {
    std::map<std::string, std::string> out;
    std::error_code ec;
    fs::path p = table_path();
    if (!fs::exists(p, ec)) return out;
    std::string text = file_util::read_text_no_bom(p);
    if (text.empty()) return out;
    json doc;
    try {
        doc = json::parse(text);
    } catch (...) {
        // Malformed table — treat as empty rather than blocking installs.
        return out;
    }
    if (!doc.is_object()) return out;
    for (auto& [k, v] : doc.items()) {
        if (v.is_string()) out[k] = v.get<std::string>();
    }
    return out;
}

}  // namespace

bool is_managed(const std::string& alias) {
    auto table = load_table();
    return table.find(alias) != table.end();
}

WriteResult write_shim(const std::string& alias,
                       const fs::path& exe,
                       const std::vector<std::string>& prefix_args,
                       bool force) {
    std::error_code ec;
    fs::create_directories(paths::bin_dir(), ec);

    fs::path cmd_path = paths::bin_dir() / (alias + ".cmd");

    // Collision check: bin_dir is shared (~/.local/bin alongside uv/pipx/etc.).
    // If <alias>.cmd already exists and we're not its owner, refuse to clobber
    // unless caller passed force=true. The shim table is our source of truth
    // for "files we own".
    if (!force && fs::exists(cmd_path, ec) && !is_managed(alias)) {
        return WriteResult::Skipped;
    }

    std::ostringstream cmd;
    cmd << "@echo off\r\n\""
        << exe.string() << "\""
        << join_prefix_cmd(prefix_args)
        << " %*\r\n";
    write_text_crlf(cmd_path, cmd.str());
    return WriteResult::Wrote;
}

int remove_shim(const std::string& alias) {
    std::error_code ec;
    int n = 0;
    // .cmd is the current format. .ps1 / extensionless / .exe are legacy or
    // sibling artifacts we still want to clean up on removal.
    for (auto suffix : {".cmd", ".ps1", "", ".exe"}) {
        fs::path p = paths::bin_dir() / (alias + suffix);
        if (fs::exists(p, ec) && fs::remove(p, ec)) ++n;
    }
    return n;
}

}  // namespace luban::shim
