#include "luban_toml.hpp"

#include <fstream>
#include <sstream>
#include <system_error>

#include "toml.hpp"

#include "log.hpp"

namespace luban::luban_toml {

namespace {

WarningLevel parse_warnings(std::string_view s) {
    if (s == "off")    return WarningLevel::Off;
    if (s == "strict") return WarningLevel::Strict;
    return WarningLevel::Normal;
}

// Accepted C++ standards. We deliberately keep this narrow:
//   17 — current floor, most vcpkg ports compile against it
//   20 — modules / coroutines / concepts territory
//   23 — luban's own default; std::format / std::expected / ranges-v3 baked in
// 14/11 are too old to be worth supporting from `luban new`. 26 isn't yet
// universally available across MinGW/clang releases — revisit when the
// compiler we ship lands -std=c++26 by default.
bool cpp_supported(int n) {
    return n == 17 || n == 20 || n == 23;
}

int validate_cpp(int requested, std::string_view source_label) {
    if (cpp_supported(requested)) return requested;
    log::warnf("luban.toml [project] cpp = {} is not supported (accepted: 17, 20, 23).",
               requested);
    log::infof("falling back to cpp = 23 (was: {} from {}).", requested, source_label);
    return 23;
}

void load_from_table(const ::toml::table& tbl, Config& out) {
    if (auto* proj = tbl["project"].as_table()) {
        if (auto v = (*proj)["default_preset"].value<std::string>())
            out.project.default_preset = *v;
        if (auto v = (*proj)["triplet"].value<std::string>())
            out.project.triplet = *v;
        // Accept both `cpp = 23` (integer) and `cpp = "23"` (string). The
        // string form is forgiving for users who copy-paste from the docs.
        if (auto v = (*proj)["cpp"].value<int64_t>()) {
            out.project.cpp = validate_cpp(static_cast<int>(*v), "[project] cpp (integer)");
        }
        if (auto v = (*proj)["cpp"].value<std::string>()) {
            try {
                out.project.cpp = validate_cpp(std::stoi(*v), "[project] cpp (string)");
            } catch (...) {
                log::warnf("luban.toml [project] cpp = \"{}\" is not a valid integer; "
                           "falling back to cpp = 23.", *v);
                out.project.cpp = 23;
            }
        }
    }
    if (auto* scaf = tbl["scaffold"].as_table()) {
        if (auto v = (*scaf)["warnings"].value<std::string>())
            out.scaffold.warnings = parse_warnings(*v);
        if (auto* arr = (*scaf)["sanitizers"].as_array()) {
            for (auto& el : *arr) {
                if (auto s = el.value<std::string>()) {
                    out.scaffold.sanitizers.push_back(*s);
                }
            }
        }
    }

    // [toolchain] — flat string→string map of component → required version.
    // Per OQ-2, this is opt-in: missing section = no pins. Each key parses
    // as a string (we accept integer values too, for cases like cmake = 423
    // typo — coerce to "423"). Unknown component names are accepted silently;
    // the runtime check (commands/build_project.cpp) maps them against the
    // installed registry and warns there.
    if (auto* tc = tbl["toolchain"].as_table()) {
        for (auto&& [key, node] : *tc) {
            // Explicit is_string / is_integer checks — toml++'s
            // value<int64_t>() will silently coerce a boolean to 0/1, which
            // is not a sensible version pin. Same for arrays/tables/dates.
            std::string version_str;
            if (node.is_string()) {
                version_str = *node.value<std::string>();
            } else if (node.is_integer()) {
                version_str = std::to_string(*node.value<int64_t>());
            } else {
                continue;  // silently skip bool/array/table/date/float
            }
            if (version_str.empty()) continue;
            out.toolchain[std::string(key.str())] = std::move(version_str);
        }
    }
}

}  // namespace

Config load(const fs::path& path) {
    Config out;
    std::error_code ec;
    if (!fs::exists(path, ec)) return out;

    try {
        ::toml::table tbl = ::toml::parse_file(path.string());
        load_from_table(tbl, out);
    } catch (const ::toml::parse_error& e) {
        log::warnf("luban.toml parse error at {}: {}", path.string(), e.what());
    }
    return out;
}

Config load_from_text(const std::string& text) {
    Config out;
    try {
        ::toml::table tbl = ::toml::parse(text);
        load_from_table(tbl, out);
    } catch (const ::toml::parse_error& e) {
        log::warnf("luban.toml parse error: {}", e.what());
    }
    return out;
}

}  // namespace luban::luban_toml
