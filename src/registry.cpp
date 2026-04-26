#include "registry.hpp"

#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>

#include "json.hpp"

#include "paths.hpp"

namespace luban::registry {

namespace {

constexpr int kSchema = 1;

using nlohmann::json;

ComponentRecord parse_record(const std::string& name, const json& j) {
    ComponentRecord r;
    r.name = name;
    auto get_str = [&](const char* k) -> std::string {
        if (!j.contains(k) || j[k].is_null()) return "";
        if (j[k].is_string()) return j[k].get<std::string>();
        return j[k].dump();
    };
    r.version = get_str("version");
    r.source = get_str("source");
    r.url = get_str("url");
    r.hash_spec = get_str("hash");
    r.toolchain_dir = get_str("toolchain_dir");
    r.architecture = j.value("architecture", std::string{"x86_64"});
    r.installed_at = get_str("installed_at");
    if (j.contains("store_keys") && j["store_keys"].is_array()) {
        for (auto& v : j["store_keys"]) r.store_keys.emplace_back(v.get<std::string>());
    }
    if (j.contains("bins") && j["bins"].is_array()) {
        for (auto& pair : j["bins"]) {
            if (pair.is_array() && pair.size() == 2) {
                r.bins.emplace_back(pair[0].get<std::string>(), pair[1].get<std::string>());
            }
        }
    }
    return r;
}

json record_to_json(const ComponentRecord& r) {
    json bins = json::array();
    for (auto& [alias, rel] : r.bins) bins.push_back(json::array({alias, rel}));
    return {
        {"version", r.version},
        {"source", r.source},
        {"url", r.url},
        {"hash", r.hash_spec},
        {"store_keys", r.store_keys},
        {"toolchain_dir", r.toolchain_dir},
        {"bins", bins},
        {"architecture", r.architecture},
        {"installed_at", r.installed_at},
    };
}

}  // namespace

std::map<std::string, ComponentRecord> load_installed() {
    std::map<std::string, ComponentRecord> out;
    auto p = paths::installed_json_path();
    std::error_code ec;
    if (!fs::exists(p, ec)) return out;
    std::ifstream in(p, std::ios::binary);
    if (!in) return out;
    json doc;
    try { in >> doc; } catch (...) { return out; }
    if (!doc.contains("components") || !doc["components"].is_object()) return out;
    for (auto& [name, info] : doc["components"].items()) {
        if (info.is_object()) out.emplace(name, parse_record(name, info));
    }
    return out;
}

std::optional<AliasHit> resolve_alias(const std::string& alias) {
    auto recs = load_installed();
    AliasHit hit;
    bool found = false;
    for (auto& [comp_name, rec] : recs) {
        for (auto& [a, rel] : rec.bins) {
            if (a != alias) continue;
            std::string norm = rel;
            for (auto& c : norm) if (c == '/' || c == '\\') c = static_cast<char>(fs::path::preferred_separator);
            fs::path exe = paths::toolchain_dir(rec.toolchain_dir) / norm;
            if (!found) {
                hit.component = comp_name;
                hit.alias = a;
                hit.exe = exe;
                found = true;
            }
            hit.all_components.push_back(comp_name);
            break;  // 同 component 内 alias 不会重，进下一个 component
        }
    }
    if (!found) return std::nullopt;
    return hit;
}

void save_installed(const std::map<std::string, ComponentRecord>& records) {
    json comps = json::object();
    for (auto& [name, rec] : records) comps[name] = record_to_json(rec);
    json doc = {{"schema", kSchema}, {"components", comps}};

    auto p = paths::installed_json_path();
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    auto tmp = p;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        // Python writes sort_keys=True, indent=2 → match for byte-stable diff.
        out << doc.dump(2, ' ', false, json::error_handler_t::strict);
    }
    fs::rename(tmp, p, ec);
    if (ec) { fs::copy_file(tmp, p, fs::copy_options::overwrite_existing, ec); fs::remove(tmp, ec); }
}

}  // namespace luban::registry
