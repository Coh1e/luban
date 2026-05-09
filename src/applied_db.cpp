// See `applied_db.hpp`.

#include "applied_db.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <system_error>

#include "paths.hpp"

namespace luban::applied_db {

namespace {

// Strip `<source>/` qualifier from a bp name. `apply` accepts qualified
// names (`main/foundation`); the applied set stores the bare name.
std::string_view strip_source(std::string_view s) {
    auto slash = s.find('/');
    return (slash != std::string_view::npos) ? s.substr(slash + 1) : s;
}

fs::path luban_state_root() {
    return paths::state_dir() / "luban";
}

bool ensure_state_root() {
    std::error_code ec;
    fs::create_directories(luban_state_root(), ec);
    return !ec;
}

std::vector<std::string> read_lines(const fs::path& p) {
    std::vector<std::string> out;
    std::ifstream in(p);
    if (!in) return out;
    std::string line;
    while (std::getline(in, line)) {
        // Strip CR for CRLF files (Windows-edited).
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        if (!line.empty()) out.push_back(std::move(line));
    }
    return out;
}

bool append_line(const fs::path& p, std::string_view line) {
    if (!ensure_state_root()) return false;
    std::ofstream out(p, std::ios::app);
    if (!out) return false;
    out << line << '\n';
    return out.good();
}

}  // namespace

fs::path applied_path()      { return luban_state_root() / "applied.txt"; }
fs::path owned_shims_path()  { return luban_state_root() / "owned-shims.txt"; }

bool is_applied(std::string_view name) {
    std::string bare(strip_source(name));
    for (auto& l : read_lines(applied_path())) {
        if (std::string_view(l) == bare) return true;
    }
    return false;
}

bool mark_applied(std::string_view name) {
    std::string bare(strip_source(name));
    if (is_applied(bare)) return true;
    return append_line(applied_path(), bare);
}

bool record_owned_shim(const fs::path& shim_abs_path) {
    return append_line(owned_shims_path(), shim_abs_path.string());
}

std::vector<fs::path> list_owned_shims() {
    std::vector<fs::path> out;
    for (auto& l : read_lines(owned_shims_path())) {
        out.emplace_back(l);
    }
    return out;
}

void clear() {
    std::error_code ec;
    fs::remove(applied_path(), ec);
    fs::remove(owned_shims_path(), ec);
}

}  // namespace luban::applied_db
