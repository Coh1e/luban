#include "file_util.hpp"

#include <fstream>
#include <sstream>
#include <system_error>

namespace luban::file_util {

namespace {

// Tolerant slurp — never throws. Empty string covers both "missing" and
// "empty file"; consumers fs::exists() first if they need to distinguish.
std::string slurp(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// EF BB BF — the UTF-8 byte-order mark. Notepad and various Scoop manifest
// editors prepend it; tolerant parsers (nlohmann::json, toml++) reject it.
// Strip in-place when present.
void strip_utf8_bom(std::string& s) {
    if (s.size() >= 3
        && static_cast<unsigned char>(s[0]) == 0xEF
        && static_cast<unsigned char>(s[1]) == 0xBB
        && static_cast<unsigned char>(s[2]) == 0xBF) {
        s.erase(0, 3);
    }
}

}  // namespace

std::string read_text(const fs::path& path) {
    return slurp(path);
}

std::string read_text_no_bom(const fs::path& path) {
    std::string s = slurp(path);
    strip_utf8_bom(s);
    return s;
}

bool write_text_atomic(const fs::path& path, std::string_view content) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    // ignore create_directories ec — if parent already exists or path has no
    // parent (relative bare name), that's fine; the open below will surface
    // any real problem.

    fs::path tmp = path; tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!out) return false;
    }

    fs::rename(tmp, path, ec);
    if (ec) {
        // Cross-volume rename returns EXDEV / ERROR_NOT_SAME_DEVICE. Fall back
        // to copy + remove so the helper works for `<state>` on D: with
        // `<config>` on C: and similar split-volume layouts.
        ec.clear();
        fs::copy_file(tmp, path, fs::copy_options::overwrite_existing, ec);
        std::error_code rm_ec;
        fs::remove(tmp, rm_ec);
    }
    return !ec;
}

}  // namespace luban::file_util
