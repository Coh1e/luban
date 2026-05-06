// Identity / lookup half of the store module — the part that doesn't need
// network or archive deps. Split out from `store_fetch.cpp` so the unit
// tests can link against just compute_artifact_id / store_path /
// is_present without dragging in WinHTTP, libcurl, miniz, etc.
//
// See `store.hpp` for design rationale.

#include "store.hpp"

#include <sstream>
#include <system_error>

#include "paths.hpp"

namespace luban::store {

namespace {

namespace fs = std::filesystem;

constexpr const char* kMarkerName = ".store-marker.json";

fs::path store_root() { return paths::data_dir() / "store"; }

/// Take the first 8 lowercase hex chars of a hash string. Accepts inputs
/// with or without a `<algo>:` prefix; if shorter than 8 chars, pads with
/// zeros (extremely unlikely in practice but keeps the result a fixed
/// width).
std::string hash8(std::string_view sha256) {
    auto colon = sha256.find(':');
    std::string_view hex = (colon == std::string_view::npos)
                               ? sha256
                               : sha256.substr(colon + 1);
    std::string out;
    out.reserve(8);
    for (char c : hex) {
        if (out.size() >= 8) break;
        // Lowercase the hex digits — the caller might give us "DEADBEEF".
        if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
        out.push_back(c);
    }
    while (out.size() < 8) out.push_back('0');
    return out;
}

/// Pass a segment through as-is, except empty becomes the caller-supplied
/// default. We deliberately don't escape hyphens — target strings like
/// "windows-x64" rely on hyphens, and downstream parsers (when they
/// arrive) can anchor on the trailing 8-char hash8 rather than splitting
/// on hyphens. Tool names that contain hyphens (e.g. "luban-shim") flow
/// through verbatim.
std::string normalize_segment(std::string_view s, std::string_view default_if_empty) {
    if (s.empty()) return std::string(default_if_empty);
    return std::string(s);
}

}  // namespace

std::string compute_artifact_id(std::string_view name, std::string_view version,
                                std::string_view target, std::string_view sha256) {
    std::ostringstream out;
    out << normalize_segment(name, "unnamed") << "-"
        << normalize_segment(version, "unversioned") << "-"
        << normalize_segment(target, "unknown") << "-"
        << hash8(sha256);
    return out.str();
}

fs::path store_path(std::string_view artifact_id) {
    return store_root() / std::string(artifact_id);
}

bool is_present(std::string_view artifact_id) {
    std::error_code ec;
    auto dir = store_path(artifact_id);
    if (!fs::is_directory(dir, ec)) return false;
    auto marker = dir / kMarkerName;
    return fs::is_regular_file(marker, ec);
}

}  // namespace luban::store
