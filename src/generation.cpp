// See `generation.hpp`.
//
// Atomic write strategy is the same tmp + rename pattern used by
// blueprint_lock and luban_cmake_gen: write to "<path>.tmp", rename
// over the target. A crash mid-write leaves the old file intact.
//
// current.txt (rather than a symlink): see header comment for
// rationale.

#include "generation.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <system_error>

#include "json.hpp"

#include "paths.hpp"

namespace luban::generation {

namespace {

namespace fs = std::filesystem;
namespace bp = luban::blueprint;
using nlohmann::json;

const char* mode_to_str(bp::FileMode m) {
    switch (m) {
        case bp::FileMode::Replace: return "replace";
        case bp::FileMode::DropIn:  return "drop-in";
    }
    return "replace";
}

std::expected<bp::FileMode, std::string> mode_from_str(std::string_view s) {
    if (s == "replace")                    return bp::FileMode::Replace;
    if (s == "drop-in" || s == "dropin")   return bp::FileMode::DropIn;
    return std::unexpected(std::string("unknown mode `") + std::string(s) + "`");
}

json tool_to_json(const ToolRecord& t) {
    json j = {
        {"from_blueprint", t.from_blueprint},
        {"artifact_id", t.artifact_id},
        {"shim_path", t.shim_path},
        {"is_external", t.is_external},
        {"external_path", t.external_path},
    };
    // Additive fields (schema 1, no version bump). Only emit when populated
    // so older readers (and diff-friendly file-on-disk views) stay quiet for
    // simple records.
    if (!t.bin_path_rel.empty()) j["bin_path_rel"] = t.bin_path_rel;
    if (!t.shim_paths_secondary.empty()) {
        j["shim_paths_secondary"] = t.shim_paths_secondary;
    }
    if (!t.bin_paths_rel_secondary.empty()) {
        j["bin_paths_rel_secondary"] = t.bin_paths_rel_secondary;
    }
    return j;
}

json file_to_json(const FileRecord& f) {
    json j = {
        {"from_blueprint", f.from_blueprint},
        {"target_path", f.target_path},
        {"content_sha256", f.content_sha256},
        {"mode", mode_to_str(f.mode)},
    };
    if (f.backup_path.has_value()) {
        j["backup_path"] = *f.backup_path;
    }
    return j;
}

std::expected<ToolRecord, std::string> tool_from_json(const json& j) {
    ToolRecord t;
    if (j.contains("from_blueprint")) t.from_blueprint = j["from_blueprint"].get<std::string>();
    if (j.contains("artifact_id"))    t.artifact_id    = j["artifact_id"].get<std::string>();
    if (j.contains("shim_path"))      t.shim_path      = j["shim_path"].get<std::string>();
    if (j.contains("is_external"))    t.is_external    = j["is_external"].get<bool>();
    if (j.contains("external_path"))  t.external_path  = j["external_path"].get<std::string>();
    // Additive fields. Absent in legacy records → leave default-empty so
    // reconcile can detect the gap and surface a warning instead of crashing.
    if (j.contains("bin_path_rel") && j["bin_path_rel"].is_string()) {
        t.bin_path_rel = j["bin_path_rel"].get<std::string>();
    }
    if (j.contains("shim_paths_secondary") && j["shim_paths_secondary"].is_array()) {
        for (auto& v : j["shim_paths_secondary"]) {
            if (v.is_string()) t.shim_paths_secondary.push_back(v.get<std::string>());
        }
    }
    if (j.contains("bin_paths_rel_secondary") && j["bin_paths_rel_secondary"].is_array()) {
        for (auto& v : j["bin_paths_rel_secondary"]) {
            if (v.is_string()) t.bin_paths_rel_secondary.push_back(v.get<std::string>());
        }
    }
    return t;
}

std::expected<FileRecord, std::string> file_from_json(const json& j) {
    FileRecord f;
    if (j.contains("from_blueprint"))  f.from_blueprint  = j["from_blueprint"].get<std::string>();
    if (j.contains("target_path"))     f.target_path     = j["target_path"].get<std::string>();
    if (j.contains("content_sha256"))  f.content_sha256  = j["content_sha256"].get<std::string>();
    std::string mode_str = j.value("mode", "replace");
    auto m = mode_from_str(mode_str);
    if (!m) return std::unexpected(m.error());
    f.mode = *m;
    if (j.contains("backup_path") && j["backup_path"].is_string()) {
        f.backup_path = j["backup_path"].get<std::string>();
    }
    return f;
}

}  // namespace

fs::path generations_dir() { return paths::state_dir() / "generations"; }

fs::path generation_path(int id) {
    return generations_dir() / (std::to_string(id) + ".json");
}

fs::path current_path() { return paths::state_dir() / "current.txt"; }

std::string now_iso8601() {
    auto t = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    struct tm gmt;
#ifdef _WIN32
    gmtime_s(&gmt, &t);
#else
    gmtime_r(&t, &gmt);
#endif
    std::ostringstream ss;
    ss << std::put_time(&gmt, "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

std::expected<Generation, std::string> read(int id) {
    auto path = generation_path(id);
    std::ifstream in(path);
    if (!in) {
        return std::unexpected("cannot open " + path.string());
    }
    std::ostringstream ss;
    ss << in.rdbuf();

    json j;
    try {
        j = json::parse(ss.str());
    } catch (const json::parse_error& e) {
        return std::unexpected(std::string("JSON parse: ") + e.what());
    }

    Generation gen;
    gen.schema = j.value("schema", 1);
    if (gen.schema != 1) {
        return std::unexpected("unsupported generation schema " +
                               std::to_string(gen.schema));
    }
    gen.id = j.value("id", id);
    gen.created_at = j.value("created_at", "");

    if (j.contains("applied_blueprints") && j["applied_blueprints"].is_array()) {
        for (auto& v : j["applied_blueprints"]) {
            if (v.is_string()) gen.applied_blueprints.push_back(v.get<std::string>());
        }
    }

    if (j.contains("tools") && j["tools"].is_object()) {
        for (auto& [name, tj] : j["tools"].items()) {
            auto t = tool_from_json(tj);
            if (!t) return std::unexpected("tool[" + name + "]: " + t.error());
            gen.tools.emplace(name, std::move(*t));
        }
    }

    if (j.contains("files") && j["files"].is_object()) {
        for (auto& [path, fj] : j["files"].items()) {
            auto f = file_from_json(fj);
            if (!f) return std::unexpected("file[" + path + "]: " + f.error());
            gen.files.emplace(path, std::move(*f));
        }
    }
    return gen;
}

std::expected<void, std::string> write(const Generation& gen) {
    auto dir = generations_dir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return std::unexpected("cannot create " + dir.string() + ": " + ec.message());

    json j;
    j["schema"] = gen.schema;
    j["id"] = gen.id;
    j["created_at"] = gen.created_at;
    j["applied_blueprints"] = gen.applied_blueprints;

    json tools = json::object();
    for (auto& [name, t] : gen.tools) tools[name] = tool_to_json(t);
    j["tools"] = std::move(tools);

    json files = json::object();
    for (auto& [path, f] : gen.files) files[path] = file_to_json(f);
    j["files"] = std::move(files);

    auto path = generation_path(gen.id);
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return std::unexpected("cannot open " + tmp.string());
        out << j.dump(2) << "\n";
        if (!out) return std::unexpected("write failure on " + tmp.string());
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        fs::rename(tmp, path, ec);
        if (ec) return std::unexpected("rename: " + ec.message());
    }
    return {};
}

std::optional<int> get_current() {
    std::ifstream in(current_path());
    if (!in) return std::nullopt;
    int id = 0;
    in >> id;
    if (!in || id < 0) return std::nullopt;
    return id;
}

std::expected<void, std::string> set_current(int id) {
    auto path = current_path();
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return std::unexpected("cannot create " + path.parent_path().string() +
                                   ": " + ec.message());
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return std::unexpected("cannot open " + tmp.string());
        out << id;
        if (!out) return std::unexpected("write failure on " + tmp.string());
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        fs::rename(tmp, path, ec);
        if (ec) return std::unexpected("rename: " + ec.message());
    }
    return {};
}

std::vector<int> list_ids() {
    std::vector<int> out;
    std::error_code ec;
    auto dir = generations_dir();
    if (!fs::is_directory(dir, ec)) return out;
    for (auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        auto stem = entry.path().stem().string();
        if (entry.path().extension() != ".json") continue;
        // Filename must be a positive integer (skip e.g. .tmp leftovers).
        try {
            size_t pos = 0;
            int id = std::stoi(stem, &pos);
            if (pos == stem.size() && id >= 0) out.push_back(id);
        } catch (...) {
            continue;
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

int highest_id() {
    auto ids = list_ids();
    return ids.empty() ? 0 : ids.back();
}

}  // namespace luban::generation
