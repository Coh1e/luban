// See `external_skip.hpp`.
//
// `probe()` does ONE thing: ask path_search::on_path where the tool
// resolves and decide whether that's "external" by checking it's not
// inside one of luban's bin dirs. This is sufficient for the common
// case (user has scoop git, doesn't want luban's mingit) and stays
// well clear of the version-parsing tarpit.

#include "external_skip.hpp"

#include <fstream>
#include <sstream>
#include <system_error>

#include "json.hpp"

#include "path_search.hpp"
#include "paths.hpp"

namespace luban::external_skip {

namespace {

namespace fs = std::filesystem;

fs::path registry_path() {
    return paths::state_dir() / "external.json";
}

/// Returns true if `resolved` lies inside any luban-owned bin directory.
/// We exclude such hits so our own shim isn't mistaken for an external
/// install when probe is called after a partial setup.
bool is_under_luban_bin(const fs::path& resolved) {
    std::error_code ec;
    auto resolved_canon = fs::weakly_canonical(resolved, ec);
    if (ec) resolved_canon = resolved;

    auto under = [&](const fs::path& root) {
        if (root.empty()) return false;
        std::error_code ec2;
        auto root_canon = fs::weakly_canonical(root, ec2);
        if (ec2) root_canon = root;
        // Compare path strings lexicographically, normalized to lower
        // on Windows (case-insensitive fs).
        auto a = resolved_canon.string();
        auto b = root_canon.string();
        for (auto& c : a) c = static_cast<char>(::tolower(c));
        for (auto& c : b) c = static_cast<char>(::tolower(c));
        return a.starts_with(b);
    };

    // <data>/bin (legacy v0.x location). We check both this and the
    // newer ~/.local/bin/ — luban v1.0+ uses XDG bin home for shim,
    // but a machine being upgraded mid-flight may still have the
    // legacy dir on PATH.
    if (under(paths::bin_dir())) return true;

    auto local_bin = paths::home() / ".local" / "bin";
    if (under(local_bin)) return true;

    return false;
}

}  // namespace

std::optional<External> probe(std::string_view tool_name) {
    auto found = path_search::on_path(tool_name);
    if (!found) return std::nullopt;
    if (is_under_luban_bin(*found)) return std::nullopt;
    return External{std::string(tool_name), *found};
}

Registry read() {
    Registry reg;
    auto path = registry_path();
    std::error_code ec;
    if (!fs::exists(path, ec)) return reg;

    std::ifstream in(path);
    if (!in) return reg;
    std::ostringstream ss;
    ss << in.rdbuf();

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(ss.str());
    } catch (...) {
        // Corrupt file → treat as empty rather than fail. Worst case
        // luban re-probes, which is cheap.
        return reg;
    }

    if (j.contains("schema") && j["schema"].is_number_integer()) {
        reg.schema = j["schema"].get<int>();
    }
    if (j.contains("tools") && j["tools"].is_object()) {
        for (auto& [k, v] : j["tools"].items()) {
            if (!v.is_object() || !v.contains("resolved_path")) continue;
            External e;
            e.tool_name = k;
            e.resolved_path =
                fs::path(v["resolved_path"].get<std::string>());
            reg.tools.emplace(k, std::move(e));
        }
    }
    return reg;
}

bool write(const Registry& reg) {
    nlohmann::json j;
    j["schema"] = reg.schema;
    nlohmann::json tools = nlohmann::json::object();
    for (auto& [name, e] : reg.tools) {
        tools[name] = {
            {"resolved_path", e.resolved_path.string()},
        };
    }
    j["tools"] = std::move(tools);

    auto path = registry_path();
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return false;

    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp);
        if (!out) return false;
        out << j.dump(2) << "\n";
        if (!out) return false;
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        fs::rename(tmp, path, ec);
        if (ec) return false;
    }
    return true;
}

}  // namespace luban::external_skip
