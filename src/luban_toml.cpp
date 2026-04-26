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

void load_from_table(const ::toml::table& tbl, Config& out) {
    if (auto* proj = tbl["project"].as_table()) {
        if (auto v = (*proj)["default_preset"].value<std::string>())
            out.project.default_preset = *v;
        if (auto v = (*proj)["triplet"].value<std::string>())
            out.project.triplet = *v;
        if (auto v = (*proj)["cpp"].value<int64_t>())
            out.project.cpp = static_cast<int>(*v);
        // 也接受 cpp = "23" 字符串
        if (auto v = (*proj)["cpp"].value<std::string>()) {
            try { out.project.cpp = std::stoi(*v); } catch (...) {}
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
