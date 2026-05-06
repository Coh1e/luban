// See `blueprint_lock.hpp`.
//
// Plain JSON read/write via the vendored nlohmann::json. Atomic writes
// use the same tmp + rename pattern as luban_cmake_gen / vcpkg_manifest:
// write the new content to "<path>.tmp", fsync (best-effort), rename
// over the target. That way a crash leaves either the old file or the
// new file fully on disk — never a half-written one.

#include "blueprint_lock.hpp"

#include <fstream>
#include <sstream>
#include <system_error>

#include "json.hpp"

namespace luban::blueprint_lock {

namespace {

namespace bp = luban::blueprint;
using nlohmann::json;

const char* mode_to_string(bp::FileMode m) {
    switch (m) {
        case bp::FileMode::Replace: return "replace";
        case bp::FileMode::DropIn:  return "drop-in";
    }
    return "replace";
}

std::expected<bp::FileMode, std::string> mode_from_string(const std::string& s) {
    if (s == "replace")                        return bp::FileMode::Replace;
    if (s == "drop-in" || s == "dropin")       return bp::FileMode::DropIn;
    return std::unexpected("unknown mode `" + s + "`");
}

}  // namespace

std::string to_string(const BlueprintLock& lock) {
    json j;
    j["schema"] = lock.schema;
    j["blueprint_name"] = lock.blueprint_name;
    j["blueprint_sha256"] = lock.blueprint_sha256;
    j["resolved_at"] = lock.resolved_at;
    j["bp_source"] = lock.bp_source;
    j["bp_source_commit"] = lock.bp_source_commit;

    json tools_json = json::object();
    for (auto& [name, t] : lock.tools) {
        json tool_json;
        tool_json["version"] = t.version;
        tool_json["source"] = t.source;
        json plats = json::object();
        for (auto& [target, p] : t.platforms) {
            plats[target] = {
                {"url", p.url},
                {"sha256", p.sha256},
                {"bin", p.bin},
                {"artifact_id", p.artifact_id},
            };
        }
        tool_json["platforms"] = std::move(plats);
        tools_json[name] = std::move(tool_json);
    }
    j["tools"] = std::move(tools_json);

    json files_json = json::object();
    for (auto& [path, f] : lock.files) {
        files_json[path] = {
            {"content_sha256", f.content_sha256},
            {"mode", mode_to_string(f.mode)},
        };
    }
    j["files"] = std::move(files_json);

    // Pretty-print with 2-space indent so users can review .lock files
    // in PRs without their eyes melting.
    return j.dump(2);
}

std::expected<BlueprintLock, std::string> read_string(std::string_view text) {
    json j;
    try {
        j = json::parse(text);
    } catch (json::parse_error& e) {
        return std::unexpected(std::string("JSON parse: ") + e.what());
    }

    BlueprintLock lock;
    if (!j.contains("schema") || !j["schema"].is_number_integer()) {
        return std::unexpected("missing or non-integer `schema`");
    }
    lock.schema = j["schema"].get<int>();
    if (lock.schema != 1) {
        return std::unexpected("unsupported schema " + std::to_string(lock.schema) +
                               " (this build supports schema 1)");
    }

    auto get_str = [&](const char* key, std::string& out) -> std::expected<void, std::string> {
        if (!j.contains(key)) return std::unexpected(std::string("missing `") + key + "`");
        if (!j[key].is_string()) return std::unexpected(std::string("`") + key + "` must be a string");
        out = j[key].get<std::string>();
        return {};
    };
    if (auto r = get_str("blueprint_name", lock.blueprint_name); !r) return std::unexpected(r.error());
    if (auto r = get_str("blueprint_sha256", lock.blueprint_sha256); !r) return std::unexpected(r.error());
    if (auto r = get_str("resolved_at", lock.resolved_at); !r) return std::unexpected(r.error());
    // bp_source / bp_source_commit are post-§9.10 additions; tolerate
    // their absence in pre-existing locks so old .lock files keep
    // round-tripping until they're rewritten by `bp apply --update`.
    if (j.contains("bp_source") && j["bp_source"].is_string()) {
        lock.bp_source = j["bp_source"].get<std::string>();
    }
    if (j.contains("bp_source_commit") && j["bp_source_commit"].is_string()) {
        lock.bp_source_commit = j["bp_source_commit"].get<std::string>();
    }

    if (j.contains("tools") && j["tools"].is_object()) {
        for (auto& [name, t] : j["tools"].items()) {
            LockedTool lt;
            if (t.contains("version"))  lt.version = t["version"].get<std::string>();
            if (t.contains("source"))   lt.source  = t["source"].get<std::string>();
            if (t.contains("platforms") && t["platforms"].is_object()) {
                for (auto& [target, p] : t["platforms"].items()) {
                    LockedPlatform lp;
                    if (p.contains("url"))         lp.url         = p["url"].get<std::string>();
                    if (p.contains("sha256"))      lp.sha256      = p["sha256"].get<std::string>();
                    if (p.contains("bin"))         lp.bin         = p["bin"].get<std::string>();
                    if (p.contains("artifact_id")) lp.artifact_id = p["artifact_id"].get<std::string>();
                    lt.platforms.emplace(target, std::move(lp));
                }
            }
            lock.tools.emplace(name, std::move(lt));
        }
    }

    if (j.contains("files") && j["files"].is_object()) {
        for (auto& [path, f] : j["files"].items()) {
            LockedFile lf;
            if (f.contains("content_sha256")) {
                lf.content_sha256 = f["content_sha256"].get<std::string>();
            }
            std::string mode_str = "replace";
            if (f.contains("mode")) mode_str = f["mode"].get<std::string>();
            auto m = mode_from_string(mode_str);
            if (!m) return std::unexpected("files[\"" + path + "\"]: " + m.error());
            lf.mode = *m;
            lock.files.emplace(path, std::move(lf));
        }
    }

    return lock;
}

std::expected<BlueprintLock, std::string> read_file(
    const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
        return std::unexpected("cannot open " + path.string());
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return read_string(ss.str());
}

std::expected<void, std::string> write_file(const std::filesystem::path& path,
                                            const BlueprintLock& lock) {
    std::filesystem::path tmp = path;
    tmp += ".tmp";

    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return std::unexpected("cannot open " + tmp.string() + " for writing");
        }
        out << to_string(lock) << "\n";
        if (!out) {
            return std::unexpected("write failure on " + tmp.string());
        }
    }

    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        // Windows can fail rename if the target is open in another process.
        // Try remove + rename as a fallback. On a real failure both calls
        // fail and we surface the original error.
        std::filesystem::remove(path, ec);
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            return std::unexpected("rename " + tmp.string() + " -> " +
                                   path.string() + ": " + ec.message());
        }
    }
    return {};
}

}  // namespace luban::blueprint_lock
