#include "vcpkg_manifest.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>

#include "log.hpp"

namespace luban::vcpkg_manifest {

namespace {

using nlohmann::json;

// "10.2" → "10.2.0" — vcpkg 的 version>= 期望 X.Y.Z 三段；不全则补 0。
// 已是三段或更多段（含 - 后缀）原样返回。
std::string normalize_version_ge(const std::string& v) {
    if (v.empty()) return v;
    int dots = 0;
    for (char c : v) if (c == '.') ++dots;
    std::string out = v;
    while (dots < 2) { out += ".0"; ++dots; }
    return out;
}

}  // namespace

Manifest load(const fs::path& path, const std::string& fallback_name) {
    Manifest m;
    m.name = fallback_name;

    std::error_code ec;
    if (!fs::exists(path, ec)) return m;

    json doc;
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) return m;
        try { in >> doc; } catch (...) { return m; }
    }
    if (!doc.is_object()) return m;

    if (doc.contains("name") && doc["name"].is_string()) {
        m.name = doc["name"].get<std::string>();
    }
    if (doc.contains("version") && doc["version"].is_string()) {
        m.version = doc["version"].get<std::string>();
    } else if (doc.contains("version-string") && doc["version-string"].is_string()) {
        // 旧 manifest 用 version-string；统一折成 version
        m.version = doc["version-string"].get<std::string>();
    }
    if (doc.contains("dependencies") && doc["dependencies"].is_array()) {
        for (auto& d : doc["dependencies"]) {
            Dependency dep;
            if (d.is_string()) {
                dep.name = d.get<std::string>();
            } else if (d.is_object() && d.contains("name") && d["name"].is_string()) {
                dep.name = d["name"].get<std::string>();
                if (d.contains("version>=") && d["version>="].is_string()) {
                    dep.version_ge = d["version>="].get<std::string>();
                }
            } else {
                continue;
            }
            m.dependencies.push_back(std::move(dep));
        }
    }

    // 保留所有未识别字段（features / overrides / supports / builtin-baseline / 等）
    json extras = json::object();
    for (auto& [k, v] : doc.items()) {
        if (k == "name" || k == "version" || k == "version-string" || k == "dependencies") continue;
        extras[k] = v;
    }
    m.extras = std::move(extras);
    return m;
}

void save(const fs::path& path, const Manifest& m) {
    json doc = json::object();
    doc["name"] = m.name;
    doc["version"] = m.version;

    json deps = json::array();
    for (auto& d : m.dependencies) {
        if (d.version_ge && !d.version_ge->empty()) {
            json obj = json::object();
            obj["name"] = d.name;
            obj["version>="] = normalize_version_ge(*d.version_ge);
            deps.push_back(obj);
        } else {
            deps.push_back(d.name);
        }
    }
    doc["dependencies"] = deps;

    if (m.extras.is_object()) {
        for (auto& [k, v] : m.extras.items()) doc[k] = v;
    }

    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    fs::path tmp = path; tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << doc.dump(2) << '\n';
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::copy_file(tmp, path, fs::copy_options::overwrite_existing, ec);
        fs::remove(tmp, ec);
    }
}

void add(Manifest& m, const std::string& pkg, const std::string& version_ge) {
    auto it = std::find_if(m.dependencies.begin(), m.dependencies.end(),
        [&](const Dependency& d) { return d.name == pkg; });
    Dependency dep;
    dep.name = pkg;
    if (!version_ge.empty()) dep.version_ge = version_ge;
    if (it != m.dependencies.end()) {
        *it = std::move(dep);
    } else {
        m.dependencies.push_back(std::move(dep));
    }
}

bool remove(Manifest& m, const std::string& pkg) {
    auto it = std::find_if(m.dependencies.begin(), m.dependencies.end(),
        [&](const Dependency& d) { return d.name == pkg; });
    if (it == m.dependencies.end()) return false;
    m.dependencies.erase(it);
    return true;
}

std::pair<std::string, std::string> parse_pkg_spec(const std::string& spec) {
    auto at = spec.find('@');
    if (at == std::string::npos) return {spec, ""};
    return {spec.substr(0, at), spec.substr(at + 1)};
}

}  // namespace luban::vcpkg_manifest
