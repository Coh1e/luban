// See `blueprint_toml.hpp`.
//
// We use the toml++ library that's already vendored at third_party/toml.hpp
// (the same parser luban_toml.cpp uses for project-level luban.toml).
// Walking a toml::table to extract our schema is straightforward; the only
// non-obvious bit is converting nested `[config.X]` tables into
// nlohmann::json so renderers downstream can consume them uniformly.

#include "blueprint_toml.hpp"

#include <fstream>
#include <sstream>

#include "toml.hpp"

namespace luban::blueprint_toml {

namespace {

namespace bp = luban::blueprint;
using nlohmann::json;

// ---- toml::node → nlohmann::json conversion -----------------------------
//
// Renderers (Lua/JS) consume program config via JSON because that's the
// universal interchange format we already vendor (json.hpp). Converting
// at the parser boundary keeps everything downstream simple.
//
// Edge cases:
// - TOML dates/times become ISO-8601 strings (lossy but practical; we
//   don't expect blueprints to contain raw datetimes anyway).
// - TOML int64 / double / bool map to JSON's same; strings verbatim.
// - Arrays and tables recurse.

json node_to_json(const ::toml::node& node);

json table_to_json(const ::toml::table& tbl) {
    json out = json::object();
    for (auto&& [k, v] : tbl) {
        out[std::string(k.str())] = node_to_json(v);
    }
    return out;
}

json array_to_json(const ::toml::array& arr) {
    json out = json::array();
    for (auto&& v : arr) {
        out.push_back(node_to_json(v));
    }
    return out;
}

json node_to_json(const ::toml::node& node) {
    if (auto* tbl = node.as_table()) return table_to_json(*tbl);
    if (auto* arr = node.as_array()) return array_to_json(*arr);
    if (auto* s = node.as_string()) return std::string(s->get());
    if (auto* i = node.as_integer()) return i->get();
    if (auto* d = node.as_floating_point()) return d->get();
    if (auto* b = node.as_boolean()) return b->get();
    // TOML dates / times / date-times — serialize via toml++'s own
    // ostream operators (defined per-type in toml.hpp). We don't expect
    // blueprints to carry these in practice, but be lenient.
    std::ostringstream ss;
    if (auto* dt = node.as_date_time()) ss << *dt;
    else if (auto* d = node.as_date())   ss << *d;
    else if (auto* t = node.as_time())   ss << *t;
    else                                  ss << "<unknown toml node>";
    return ss.str();
}

// ---- field extractors with explicit error reporting --------------------

template <typename T>
std::optional<T> opt_value(const ::toml::table& tbl, std::string_view key) {
    return tbl[key].value<T>();
}

// Schema validation helper: accumulate errors instead of failing fast,
// so a typo in one section doesn't hide schema errors elsewhere.
struct ParseCtx {
    bp::BlueprintSpec spec;
    std::vector<std::string> errors;

    void err(std::string msg) { errors.push_back(std::move(msg)); }

    bool ok() const { return errors.empty(); }

    std::string joined_errors() const {
        std::string out;
        for (auto& e : errors) {
            out += e;
            out += "\n";
        }
        if (!out.empty()) out.pop_back();
        return out;
    }
};

void parse_tools(const ::toml::table* tools_tbl, ParseCtx& ctx) {
    if (!tools_tbl) return;  // [tool] section is optional
    for (auto&& [name_key, tool_node] : *tools_tbl) {
        std::string tool_name(name_key.str());
        bp::ToolSpec tool;
        tool.name = tool_name;

        auto* tool_tbl = tool_node.as_table();
        if (!tool_tbl) {
            ctx.err("[tool." + tool_name + "] must be a table");
            continue;
        }

        if (auto v = (*tool_tbl)["source"].value<std::string>())        tool.source        = *v;
        if (auto v = (*tool_tbl)["version"].value<std::string>())       tool.version       = *v;
        if (auto v = (*tool_tbl)["bin"].value<std::string>())           tool.bin           = *v;
        if (auto v = (*tool_tbl)["post_install"].value<std::string>())  tool.post_install  = *v;
        if (auto v = (*tool_tbl)["external_skip"].value<std::string>()) tool.external_skip = *v;

        // `shims` accepts either a single string (shorthand for one shim)
        // or an array of strings (multi-binary tools). Type errors must
        // surface as schema errors — silent-drop here would have masked
        // exactly the openssh-blueprint footgun this commit fixes.
        auto shims_view = (*tool_tbl)["shims"];
        if (auto* arr = shims_view.as_array()) {
            for (auto&& item : *arr) {
                if (auto* item_s = item.as_string()) {
                    tool.shims.push_back(std::string(item_s->get()));
                } else {
                    ctx.err("[tool." + tool_name +
                            "] shims entries must be strings");
                }
            }
        } else if (auto* s = shims_view.as_string()) {
            tool.shims.push_back(std::string(s->get()));
        } else if (shims_view) {
            ctx.err("[tool." + tool_name +
                    "] shims must be a string or array of strings");
        }

        if (auto v = (*tool_tbl)["shim_dir"].value<std::string>()) tool.shim_dir = *v;
        if (auto v = (*tool_tbl)["no_shim"].value<bool>())          tool.no_shim  = *v;

        // [[tool.X.platform]] inline blocks (manual mode).
        if (auto* platforms = (*tool_tbl)["platform"].as_array()) {
            for (auto&& p_node : *platforms) {
                auto* p_tbl = p_node.as_table();
                if (!p_tbl) {
                    ctx.err("[[tool." + tool_name + ".platform]] entry must be a table");
                    continue;
                }
                bp::PlatformSpec ps;
                if (auto v = (*p_tbl)["target"].value<std::string>()) ps.target = *v;
                if (auto v = (*p_tbl)["url"].value<std::string>())    ps.url    = *v;
                if (auto v = (*p_tbl)["sha256"].value<std::string>()) ps.sha256 = *v;
                if (auto v = (*p_tbl)["bin"].value<std::string>())    ps.bin    = *v;

                if (ps.target.empty()) {
                    ctx.err("[[tool." + tool_name + ".platform]] missing required `target`");
                }
                if (ps.url.empty()) {
                    ctx.err("[[tool." + tool_name + ".platform]] missing required `url`");
                }
                if (ps.sha256.empty()) {
                    ctx.err("[[tool." + tool_name + ".platform]] missing required `sha256`");
                }
                tool.platforms.push_back(std::move(ps));
            }
        }

        // Either source must be set OR at least one platform must be present.
        if (!tool.source.has_value() && tool.platforms.empty()) {
            ctx.err("[tool." + tool_name + "] needs either `source` or [[platform]] inline");
        }

        ctx.spec.tools.push_back(std::move(tool));
    }
}

void parse_configs(const ::toml::table* configs_tbl, ParseCtx& ctx) {
    if (!configs_tbl) return;
    for (auto&& [name_key, cfg_node] : *configs_tbl) {
        std::string cfg_name(name_key.str());
        auto* cfg_tbl = cfg_node.as_table();
        if (!cfg_tbl) {
            ctx.err("[config." + cfg_name + "] must be a table");
            continue;
        }
        bp::ConfigSpec cfg;
        cfg.name = cfg_name;
        // for_tool is a parser-level field, not part of the tool's own
        // cfg schema. Pull it out, then prune it from the JSON view we
        // hand to the renderer so cfg looks clean. Erasing on JSON
        // (post-conversion) sidesteps toml::table copyability concerns.
        if (auto v = (*cfg_tbl)["for_tool"].value<std::string>()) {
            cfg.for_tool = *v;
        }
        cfg.config = table_to_json(*cfg_tbl);
        if (cfg.config.is_object()) cfg.config.erase("for_tool");
        ctx.spec.configs.push_back(std::move(cfg));
    }
}

void parse_files(const ::toml::table* files_tbl, ParseCtx& ctx) {
    if (!files_tbl) return;
    for (auto&& [path_key, file_node] : *files_tbl) {
        std::string target_path(path_key.str());
        auto* file_tbl = file_node.as_table();
        if (!file_tbl) {
            ctx.err("[file.\"" + target_path + "\"] must be a table");
            continue;
        }
        bp::FileSpec f;
        f.target_path = target_path;
        if (auto v = (*file_tbl)["content"].value<std::string>()) f.content = *v;

        // mode default: replace. Unknown values are an error so users
        // catch typos at parse time, not at apply time.
        std::string mode = "replace";
        if (auto v = (*file_tbl)["mode"].value<std::string>()) mode = *v;
        if (mode == "replace") {
            f.mode = bp::FileMode::Replace;
        } else if (mode == "drop-in" || mode == "dropin") {
            f.mode = bp::FileMode::DropIn;
        } else if (mode == "merge") {
            f.mode = bp::FileMode::Merge;
        } else if (mode == "append") {
            f.mode = bp::FileMode::Append;
        } else {
            ctx.err("[file.\"" + target_path + "\"] unknown mode `" + mode +
                    "` (expected: replace | drop-in | merge | append)");
            continue;
        }
        if (f.content.empty()) {
            ctx.err("[file.\"" + target_path + "\"] missing required `content`");
            continue;
        }
        ctx.spec.files.push_back(std::move(f));
    }
}

void parse_meta(const ::toml::table* meta_tbl, ParseCtx& ctx) {
    if (!meta_tbl) return;
    if (auto* arr = (*meta_tbl)["requires"].as_array()) {
        for (auto&& v : *arr) {
            if (auto* s = v.as_string()) {
                ctx.spec.meta.requires_.push_back(std::string(s->get()));
            }
        }
    }
    if (auto* arr = (*meta_tbl)["conflicts"].as_array()) {
        for (auto&& v : *arr) {
            if (auto* s = v.as_string()) {
                ctx.spec.meta.conflicts.push_back(std::string(s->get()));
            }
        }
    }
}

std::expected<bp::BlueprintSpec, std::string> parse_table(const ::toml::table& tbl) {
    ParseCtx ctx;
    ctx.spec.schema = static_cast<int>(tbl["schema"].value_or<int64_t>(1));

    if (auto v = tbl["name"].value<std::string>()) ctx.spec.name = *v;
    else ctx.err("missing required top-level `name`");

    if (auto v = tbl["description"].value<std::string>()) ctx.spec.description = *v;

    parse_tools(tbl["tool"].as_table(), ctx);
    parse_configs(tbl["config"].as_table(), ctx);
    parse_files(tbl["file"].as_table(), ctx);
    parse_meta(tbl["meta"].as_table(), ctx);

    if (!ctx.ok()) return std::unexpected(ctx.joined_errors());
    return std::move(ctx.spec);
}

}  // namespace

std::expected<bp::BlueprintSpec, std::string> parse_file(
    const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
        return std::unexpected("cannot open " + path.string());
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return parse_string(ss.str());
}

std::expected<bp::BlueprintSpec, std::string> parse_string(
    std::string_view content) {
    // The vendored toml++ throws on parse error rather than returning a
    // parse_result. Wrap to convert that to our expected<> contract.
    try {
        ::toml::table tbl = ::toml::parse(content);
        return parse_table(tbl);
    } catch (const ::toml::parse_error& e) {
        return std::unexpected(std::string("TOML syntax: ") + e.what());
    }
}

}  // namespace luban::blueprint_toml
