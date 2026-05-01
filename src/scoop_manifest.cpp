#include "scoop_manifest.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <format>

namespace luban::scoop_manifest {

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

// 拒绝执行的 Scoop 字段 — 它们要 PowerShell 沙盒能力，luban 不给。
constexpr std::array<const char*, 6> kDeniedFields = {
    "installer", "pre_install", "post_install",
    "uninstaller", "persist", "psmodule",
};

// Scoop 的 architecture 键 → linux-style triple
struct ArchMap { const char* scoop; const char* triple; };
constexpr std::array<ArchMap, 3> kScoopArch = {{
    {"64bit",  "x86_64"},
    {"32bit",  "x86"},
    {"arm64",  "aarch64"},
}};

const char* arch_to_scoop_key(const std::string& arch) {
    for (auto& m : kScoopArch) {
        if (arch == m.triple) return m.scoop;
    }
    return nullptr;
}

std::string trim(std::string s) {
    auto not_ws = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_ws));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_ws).base(), s.end());
    return s;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

void check_no_denied(const json& obj, const std::string& name, const std::string& where) {
    std::vector<std::string> bad;
    for (auto& field : kDeniedFields) {
        if (obj.contains(field)) bad.emplace_back(field);
    }
    if (!bad.empty()) {
        std::sort(bad.begin(), bad.end());
        std::string joined;
        for (size_t i = 0; i < bad.size(); ++i) {
            if (i) joined += ", ";
            joined += bad[i];
        }
        std::string msg = std::format(
            "manifest '{}'{} uses fields we refuse to execute: {}. "
            "Provide an overlay manifest with a plain url+hash instead.",
            name, where.empty() ? "" : (" (" + where + ")"), joined);
        throw UnsafeManifest(msg);
    }
}

// Scoop 的 url / hash 都允许字符串或单元素列表；多元素列表暂不支持，
// 让用户写 overlay 改成单一 url。
std::string normalize_str_or_singleton_list(const json& raw, const std::string& name,
                                            const char* what) {
    if (raw.is_array()) {
        if (raw.size() != 1) {
            throw IncompleteManifest(std::format(
                "manifest '{}' has multi-{} list; needs overlay", name, what));
        }
        return raw[0].is_string() ? raw[0].get<std::string>() : "";
    }
    if (!raw.is_string()) {
        throw IncompleteManifest(std::format(
            "manifest '{}' has empty {}", name, what));
    }
    return raw.get<std::string>();
}

std::string normalize_hash(const json& raw, const std::string& name) {
    std::string s = trim(normalize_str_or_singleton_list(raw, name, "hash"));
    if (s.empty()) throw IncompleteManifest(std::format("manifest '{}' has empty hash", name));
    if (s.find(':') == std::string::npos) s = "sha256:" + s;
    return lower(std::move(s));
}

std::string normalize_url(const json& raw, const std::string& name) {
    std::string s = trim(normalize_str_or_singleton_list(raw, name, "url"));
    if (s.empty()) throw IncompleteManifest(std::format("manifest '{}' has empty url", name));
    // Scoop 允许 "url.zip#/dest.zip" 重命名后缀——我们接受但忽略。
    auto hash = s.find('#');
    if (hash != std::string::npos) s = s.substr(0, hash);
    return trim(std::move(s));
}

std::optional<std::string> as_string_or_first(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_array() && !v.empty() && v[0].is_string()) return v[0].get<std::string>();
    return std::nullopt;
}

std::optional<BinEntry> coerce_bin_entry(const json& v) {
    if (v.is_string()) {
        std::string rel = v.get<std::string>();
        return BinEntry{rel, fs::path(rel).stem().string(), {}};
    }
    if (v.is_array() && !v.empty()) {
        if (!v[0].is_string()) return std::nullopt;
        std::string rel = v[0].get<std::string>();
        std::string alias = (v.size() > 1 && v[1].is_string())
                                ? v[1].get<std::string>()
                                : fs::path(rel).stem().string();
        std::vector<std::string> prefix;
        if (v.size() > 2 && v[2].is_string()) {
            std::string s = trim(v[2].get<std::string>());
            if (!s.empty()) {
                // 按空白切分（与 Python str.split() 等价）
                size_t start = 0;
                while (start < s.size()) {
                    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
                    size_t end = start;
                    while (end < s.size() && !std::isspace(static_cast<unsigned char>(s[end]))) ++end;
                    if (start < end) prefix.push_back(s.substr(start, end - start));
                    start = end;
                }
            }
        }
        return BinEntry{rel, alias, prefix};
    }
    return std::nullopt;
}

std::vector<BinEntry> coerce_bins(const json& raw) {
    std::vector<BinEntry> out;
    if (raw.is_null()) return out;
    if (raw.is_string()) {
        if (auto e = coerce_bin_entry(raw)) out.push_back(*e);
        return out;
    }
    if (!raw.is_array()) return out;
    // bin 可以是 [string, ...] 或 [[rel,alias], ...]——按第一个元素类型决定。
    bool is_list_of_lists = !raw.empty() && raw[0].is_array();
    if (is_list_of_lists) {
        for (auto& item : raw) {
            if (auto e = coerce_bin_entry(item)) out.push_back(*e);
        }
    } else {
        // 顶层是 string list 或单个 [rel,alias]
        if (!raw.empty() && raw[0].is_string()) {
            // 可能是单个 [rel, alias] (Scoop 的常见简写) 或 多个 strings
            // Scoop 语义：list of strings 等价于多 bin。
            for (auto& item : raw) {
                if (auto e = coerce_bin_entry(item)) out.push_back(*e);
            }
        } else {
            for (auto& item : raw) {
                if (auto e = coerce_bin_entry(item)) out.push_back(*e);
            }
        }
    }
    return out;
}

std::vector<std::string> coerce_str_list(const json& raw) {
    std::vector<std::string> out;
    if (raw.is_null()) return out;
    if (raw.is_string()) { out.push_back(raw.get<std::string>()); return out; }
    if (raw.is_array()) {
        for (auto& v : raw) if (v.is_string()) out.push_back(v.get<std::string>());
    }
    return out;
}

std::map<std::string, std::string> coerce_env_set(const json& raw) {
    std::map<std::string, std::string> out;
    if (!raw.is_object()) return out;
    for (auto& [k, v] : raw.items()) {
        out[k] = v.is_string() ? v.get<std::string>() : v.dump();
    }
    return out;
}

bool ends_with(const std::string& s, std::string_view suffix) {
    return s.size() >= suffix.size()
        && std::equal(suffix.rbegin(), suffix.rend(), s.rbegin());
}

}  // namespace

ResolvedManifest parse(const json& manifest,
                       const std::string& name,
                       const std::string& arch) {
    if (!manifest.is_object()) {
        throw IncompleteManifest(std::format("manifest '{}' is not a JSON object", name));
    }
    check_no_denied(manifest, name, "");

    std::string version = "unknown";
    if (manifest.contains("version") && manifest["version"].is_string()) {
        std::string v = trim(manifest["version"].get<std::string>());
        if (!v.empty()) version = v;
    }

    // 选 arch 子块（如果存在）。
    const json* arch_block = nullptr;
    if (manifest.contains("architecture") && manifest["architecture"].is_object()) {
        const json& arch_map = manifest["architecture"];
        const char* scoop_key = arch_to_scoop_key(arch);
        if (scoop_key && arch_map.contains(scoop_key) && arch_map[scoop_key].is_object()) {
            arch_block = &arch_map[scoop_key];
            check_no_denied(*arch_block, name, std::string("arch=") + scoop_key);
        }
    }

    auto pick = [&](const char* field) -> json {
        if (arch_block && arch_block->contains(field)) return (*arch_block)[field];
        if (manifest.contains(field)) return manifest[field];
        return json(nullptr);
    };

    std::string url = normalize_url(pick("url"), name);
    std::string hash_spec = normalize_hash(pick("hash"), name);

    auto extract_dir = as_string_or_first(pick("extract_dir"));
    auto extract_to  = as_string_or_first(pick("extract_to"));

    std::vector<BinEntry> bins = coerce_bins(pick("bin"));
    std::map<std::string, std::string> env_set = coerce_env_set(
        manifest.contains("env_set") ? manifest["env_set"] : json::object());
    std::vector<std::string> env_add_path = coerce_str_list(
        manifest.contains("env_add_path") ? manifest["env_add_path"] : json(nullptr));
    std::vector<std::string> depends = coerce_str_list(
        manifest.contains("depends") ? manifest["depends"] : json(nullptr));

    // msi/nsis 静默拒绝——需 overlay 改成 zip 等可控格式。
    std::string lurl = lower(url);
    if (ends_with(lurl, ".msi") || ends_with(lurl, ".nsis")) {
        throw UnsafeManifest(std::format(
            "manifest '{}' points to {}; msi/nsis are not supported — overlay required.",
            name, url));
    }

    // luban-specific extension: `luban_mirrors` is an array of fallback URLs
    // tried after `url` fails. Useful for mirror sites under restricted
    // networks (e.g., ghproxy.com prefixing GitHub release URLs). Hash
    // verification still applies — same bytes regardless of source.
    std::vector<std::string> mirrors;
    auto pick_mirrors = [&]() -> json {
        if (arch_block && arch_block->contains("luban_mirrors")) return (*arch_block)["luban_mirrors"];
        if (manifest.contains("luban_mirrors")) return manifest["luban_mirrors"];
        return json(nullptr);
    };
    if (json m = pick_mirrors(); m.is_array()) {
        for (auto& el : m) {
            if (!el.is_string()) continue;
            std::string s = trim(el.get<std::string>());
            if (s.empty()) continue;
            // Same '#dest.zip' rename suffix as primary URL — strip if present.
            auto h = s.find('#');
            if (h != std::string::npos) s = s.substr(0, h);
            mirrors.push_back(std::move(s));
        }
    }

    return ResolvedManifest{
        .name = name,
        .version = version,
        .url = url,
        .hash_spec = hash_spec,
        .extract_dir = extract_dir,
        .extract_to = extract_to,
        .bins = std::move(bins),
        .env_set = std::move(env_set),
        .env_add_path = std::move(env_add_path),
        .depends = std::move(depends),
        .architecture = arch,
        .mirrors = std::move(mirrors),
    };
}

}  // namespace luban::scoop_manifest
