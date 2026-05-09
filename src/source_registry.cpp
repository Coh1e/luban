// See `source_registry.hpp`.
//
// Reader uses toml++ for parsing tolerance (extra keys are ignored, not
// fatal — users may experiment with new fields ahead of luban catching up).
// Writer hand-rolls the format so that the on-disk text is identical
// regardless of toml++ version drift.

#include "source_registry.hpp"

#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>

#include "toml.hpp"

#include "paths.hpp"

namespace luban::source_registry {

namespace {

namespace fs = std::filesystem;

std::string get_str(const ::toml::table& t, const char* key) {
    if (auto v = t[key].value<std::string>()) return *v;
    return {};
}

// GitHub owners whose sources luban treats as official (DESIGN §8). Hard-
// coded; remote fetch is a chicken-and-egg problem (we can't bootstrap
// trust from the network we're about to fetch). Adding to this list is
// a deliberate code change with PR review.
constexpr std::string_view kOfficialOwners[] = {
    "Coh1e",
};

}  // namespace

bool is_official_url(std::string_view url) {
    constexpr std::string_view kPrefix = "https://github.com/";
    if (!url.starts_with(kPrefix)) return false;
    auto rest = url.substr(kPrefix.size());
    auto slash = rest.find('/');
    if (slash == std::string_view::npos || slash == 0) return false;
    auto owner = rest.substr(0, slash);
    for (auto official : kOfficialOwners) {
        if (owner == official) return true;
    }
    return false;
}

std::expected<std::vector<SourceEntry>, std::string>
read_file(const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return std::vector<SourceEntry>{};
    }
    std::ifstream in(path);
    if (!in) {
        return std::unexpected("cannot open " + path.string());
    }
    std::ostringstream ss;
    ss << in.rdbuf();

    ::toml::table tbl;
    try {
        tbl = ::toml::parse(ss.str());
    } catch (const ::toml::parse_error& e) {
        return std::unexpected("parse " + path.string() + ": " +
                               std::string(e.description()));
    }

    std::vector<SourceEntry> out;
    auto* sources = tbl["source"].as_table();
    if (!sources) return out;

    out.reserve(sources->size());
    for (auto&& [key, node] : *sources) {
        auto* entry_tbl = node.as_table();
        if (!entry_tbl) continue;
        SourceEntry e;
        e.name = std::string(key.str());
        e.url      = get_str(*entry_tbl, "url");
        e.ref      = get_str(*entry_tbl, "ref");
        e.commit   = get_str(*entry_tbl, "commit");
        e.added_at = get_str(*entry_tbl, "added_at");
        // `official` was added post-v1.0. Old entries lack it — derive
        // from URL so the user doesn't have to re-add their sources after
        // upgrading. Explicit `official = true/false` overrides the
        // derivation (lets a user pin a source as non-official locally
        // even if a future luban update adds the owner to kOfficialOwners).
        if (auto v = (*entry_tbl)["official"].value<bool>()) {
            e.official = *v;
        } else {
            e.official = is_official_url(e.url);
        }
        if (!e.url.empty()) out.push_back(std::move(e));
    }
    return out;
}

namespace {

// Inline TOML string escape — minimal subset sufficient for URLs / refs /
// timestamps / commit shas (no embedded newlines, no nested quotes).
std::string toml_quote(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

}  // namespace

std::expected<void, std::string>
write_file(const fs::path& path, const std::vector<SourceEntry>& entries) {
    std::ostringstream out;
    out << "# luban bp source registry — see DESIGN.md §9.10.\n";
    out << "# Edit by hand if you must, but `luban bp source add/rm/update`\n";
    out << "# is the supported path; commit fields stale fast otherwise.\n\n";
    for (auto& e : entries) {
        out << "[source." << e.name << "]\n";
        out << "url      = " << toml_quote(e.url) << "\n";
        out << "ref      = " << toml_quote(e.ref) << "\n";
        out << "commit   = " << toml_quote(e.commit) << "\n";
        out << "added_at = " << toml_quote(e.added_at) << "\n";
        out << "official = " << (e.official ? "true" : "false") << "\n\n";
    }

    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return std::unexpected("cannot open " + tmp.string());
        f << out.str();
        if (!f) return std::unexpected("write failure on " + tmp.string());
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        // Same Windows fallback as blueprint_lock::write_file: target may be
        // open in another process. Remove + retry surfaces a clean error
        // when both fail.
        fs::remove(path, ec);
        fs::rename(tmp, path, ec);
        if (ec) return std::unexpected("rename " + tmp.string() + " -> " +
                                       path.string() + ": " + ec.message());
    }
    return {};
}

std::expected<std::vector<SourceEntry>, std::string> read() {
    return read_file(luban::paths::bp_sources_registry_path());
}

std::expected<void, std::string> write(const std::vector<SourceEntry>& entries) {
    return write_file(luban::paths::bp_sources_registry_path(), entries);
}

std::optional<SourceEntry> find(const std::vector<SourceEntry>& entries,
                                std::string_view name) {
    for (auto& e : entries) {
        if (e.name == name) return e;
    }
    return std::nullopt;
}

bool is_valid_name(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    for (char c : name) {
        bool ok = std::isalnum(static_cast<unsigned char>(c)) ||
                  c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

bool is_reserved_name(std::string_view name) {
    // `embedded` was the legacy v0.x prefix; `local` was the conceptual
    // user-local namespace. Neither is in active use as a source today,
    // but we keep them reserved so a future re-introduction won't shadow
    // a user's existing registration. `main` used to be reserved too —
    // it now denotes the conventional foundation source name and is
    // freely usable (DESIGN §9.10 议题 AG).
    return name == "embedded" || name == "local";
}

}  // namespace luban::source_registry
