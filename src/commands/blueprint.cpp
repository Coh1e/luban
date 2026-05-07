// `luban blueprint` — the v1.0 verb family for managing user-authored
// blueprints (DESIGN.md §10, §12, §16).
//
// Subcommands implemented:
//   apply <name>      — fetch tools + render programs + deploy files,
//                       write generation N+1, set current
//   unapply <name>    — drop a blueprint's contributions; restore disk
//                       (file_deploy::restore + xdg_shim::remove_cmd_shim)
//                       and write N+1
//   ls                — list known blueprints + active generation summary
//   status            — show current generation contents
//   rollback [N]      — reconcile disk to N via blueprint_reconcile, then
//                       flip current to N (or N-1)
//
// Subcommands still deferred (out of scope for this commit):
//   update [<name>]   — re-resolve sources, rewrite lock
//   gc                — prune unreferenced store entries / old generations
//   install <tool>    — quick-add a tool into a blueprint file
//   init <name>       — scaffold a new blueprint file
//
// Blueprint file lookup convention: <config>/luban/blueprints/<name>.{toml,lua}
// — the directory part comes from paths::config_dir().

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "../blueprint.hpp"
#include "../blueprint_apply.hpp"
#include "../blueprint_lock.hpp"
#include "../blueprint_lua.hpp"
#include "../blueprint_reconcile.hpp"
#include "../blueprint_toml.hpp"
#include "../cli.hpp"
#include "../file_deploy.hpp"
#include "../generation.hpp"
#include "../log.hpp"
#include "../lua_engine.hpp"
#include "../paths.hpp"
#include "../renderer_registry.hpp"
#include "../resolver_registry.hpp"
#include "../source_registry.hpp"
#include "../source_resolver.hpp"
#include "../xdg_shim.hpp"
#include "bp_source.hpp"

namespace luban::commands {

namespace {

namespace fs = std::filesystem;
namespace bp = luban::blueprint;
namespace bpl = luban::blueprint_lock;
namespace gen = luban::generation;

/// `<config>/blueprints/`. (paths::config_dir() already includes the
/// "luban" app-name segment per src/paths.cpp; the prefix here is just
/// "blueprints/".)
fs::path blueprints_dir() {
    return paths::config_dir() / "blueprints";
}

/// Find the blueprint source file for `name`. Tries `.lua` then `.toml`
/// in <config>/luban/blueprints/. Returns the chosen path or empty.
fs::path find_blueprint_source(std::string_view name) {
    std::error_code ec;
    auto base = blueprints_dir();
    fs::path lua_path = base / (std::string(name) + ".lua");
    if (fs::is_regular_file(lua_path, ec)) return lua_path;
    fs::path toml_path = base / (std::string(name) + ".toml");
    if (fs::is_regular_file(toml_path, ec)) return toml_path;
    return {};
}

/// Resolve a blueprint name to a parsed BlueprintSpec. Three name shapes:
///   - `embedded:<X>`     — built-in (deprecated; warns on use, see §9.10 / 议题 AG)
///   - `<source>/<X>`     — registered bp source (DESIGN §9.10)
///   - `<X>`              — bare; search user-local first, then registered
///                          sources in registration order. Multi-source
///                          collision is fatal: caller must qualify.
struct ResolvedBlueprint {
    bp::BlueprintSpec spec;
    fs::path source_path;     ///< Empty for embedded:.
    bool is_embedded = false;
    std::string bp_source;    ///< Registered source name when matched there.
};

namespace sr = luban::source_registry;

/// Locate a `<bp-name>.{lua,toml}` inside the on-disk dir of a registered
/// source. Returns empty path if no match.
fs::path find_blueprint_in_source_dir(const fs::path& source_root,
                                      std::string_view name) {
    auto base = source_root / "blueprints";
    std::error_code ec;
    fs::path lua_path = base / (std::string(name) + ".lua");
    if (fs::is_regular_file(lua_path, ec)) return lua_path;
    fs::path toml_path = base / (std::string(name) + ".toml");
    if (fs::is_regular_file(toml_path, ec)) return toml_path;
    return {};
}

fs::path source_root_for(const sr::SourceEntry& e) {
    constexpr std::string_view kFileScheme = "file://";
    if (std::string_view(e.url).starts_with(kFileScheme)) {
        // Mirror parse_file_url() in commands/bp_source.cpp. file:// sources
        // are live-linked: blueprints live straight inside the user's repo.
        std::string_view rest = std::string_view(e.url).substr(kFileScheme.size());
        if (rest.starts_with("/") && rest.size() >= 4 &&
            std::isalpha(static_cast<unsigned char>(rest[1])) && rest[2] == ':') {
            rest.remove_prefix(1);
        }
        return fs::path(std::string(rest));
    }
    return paths::bp_sources_dir(e.name);
}

std::expected<bp::BlueprintSpec, std::string> parse_by_extension(const fs::path& src) {
    auto ext = src.extension().string();
    if (ext == ".lua")  return luban::blueprint_lua::parse_file(src);
    if (ext == ".toml") return luban::blueprint_toml::parse_file(src);
    return std::unexpected("unsupported blueprint extension `" + ext + "`");
}

std::expected<ResolvedBlueprint, std::string> resolve_blueprint(
    std::string_view name) {
    constexpr std::string_view kEmbeddedPrefix = "embedded:";
    if (name.starts_with(kEmbeddedPrefix)) {
        // §9.10 议题 AG (2026-05-06): the binary embeds zero blueprints.
        // `embedded:` was a deprecation alias during transition; now a
        // hard error pointing at the new model.
        std::string_view bare = name.substr(kEmbeddedPrefix.size());
        return std::unexpected(
            "`embedded:" + std::string(bare) + "` was removed in luban v1.0;\n"
            "  bps now come from registered sources. Try:\n"
            "    luban bp src add <url>          # e.g. Coh1e/luban-bps\n"
            "    luban bp apply <source>/" + std::string(bare));
    }

    // <source>/<bp-name>: explicit qualification. Always wins over user-local.
    if (auto slash = name.find('/'); slash != std::string_view::npos) {
        std::string source_name(name.substr(0, slash));
        std::string bp_name(name.substr(slash + 1));

        auto entries = sr::read();
        if (!entries) return std::unexpected(entries.error());
        auto entry = sr::find(*entries, source_name);
        if (!entry) {
            return std::unexpected("no bp source named `" + source_name +
                                   "` (try `luban bp src ls`)");
        }
        fs::path src = find_blueprint_in_source_dir(source_root_for(*entry), bp_name);
        if (src.empty()) {
            return std::unexpected("source `" + source_name +
                                   "` has no blueprint named `" + bp_name +
                                   "` under " + (source_root_for(*entry) / "blueprints").string());
        }
        auto spec = parse_by_extension(src);
        if (!spec) return std::unexpected(spec.error());
        ResolvedBlueprint out;
        out.spec = std::move(*spec);
        out.source_path = src;
        out.bp_source = source_name;
        return out;
    }

    // Bare name: user-local first.
    fs::path src = find_blueprint_source(name);
    if (!src.empty()) {
        auto spec = parse_by_extension(src);
        if (!spec) return std::unexpected(spec.error());
        ResolvedBlueprint out;
        out.spec = std::move(*spec);
        out.source_path = src;
        return out;
    }

    // Fallback: walk registered sources in order.
    // Multiple matches across sources → ambiguous, force qualification.
    std::vector<std::pair<std::string, fs::path>> file_matches;
    auto entries = sr::read();
    if (!entries) return std::unexpected(entries.error());
    for (auto& e : *entries) {
        fs::path m = find_blueprint_in_source_dir(source_root_for(e), name);
        if (!m.empty()) file_matches.emplace_back(e.name, m);
    }

    if (file_matches.size() > 1) {
        std::string msg = "blueprint `" + std::string(name) +
                          "` is ambiguous; found in:";
        for (auto& [src_name, _] : file_matches) msg += "\n  - " + src_name + "/" + std::string(name);
        msg += "\nqualify with <source>/<bp>";
        return std::unexpected(msg);
    }
    if (file_matches.size() == 1) {
        auto spec = parse_by_extension(file_matches[0].second);
        if (!spec) return std::unexpected(spec.error());
        ResolvedBlueprint out;
        out.spec = std::move(*spec);
        out.source_path = file_matches[0].second;
        out.bp_source = file_matches[0].first;
        return out;
    }

    return std::unexpected("no blueprint named `" + std::string(name) +
                           "` under " + blueprints_dir().string() +
                           " or any registered source (try `luban bp search " +
                           std::string(name) + "`)");
}

/// Lockfile companion path for a blueprint source: `<src>.lock` always
/// (so foo.toml → foo.toml.lock; foo.lua → foo.lua.lock).
fs::path lock_path_for(const fs::path& source) {
    auto out = source;
    out += ".lock";
    return out;
}

/// Read the lock file for a blueprint, or build a fresh one from the
/// spec via source_resolver. With `force_resolve = true`, the on-disk
/// lock is ignored and we always re-resolve (the `bp apply --update`
/// path: pick up newer GitHub releases / re-pin sha256s).
///
/// `source` may be empty when the blueprint came from an embedded
/// builtin; in that case we skip the on-disk lock lookup entirely and
/// always fall through to fresh resolution.
std::expected<bpl::BlueprintLock, std::string> resolve_lock(
    const fs::path& source, const bp::BlueprintSpec& spec,
    bool force_resolve,
    const luban::resolver_registry::ResolverRegistry* sreg = nullptr) {
    if (!source.empty() && !force_resolve) {
        auto lp = lock_path_for(source);
        std::error_code ec;
        if (fs::is_regular_file(lp, ec)) {
            return luban::blueprint_lock::read_file(lp);
        }
    }

    // No lock (or --update) — re-resolve every tool via source_resolver.
    // Failures (e.g. a github asset scorer miss) soft-skip with a
    // warning so the rest of the blueprint (programs / files / tools
    // with inline platforms) still applies. apply() subsequently skips
    // the same tools because they have no lock entry.
    //
    // `sreg` is the bp-registered resolver registry (Tier 1, v0.4.x).
    // For Lua bps that called `luban.register_resolver(scheme, fn)`
    // during parse, this lets resolver dispatch find the user-supplied
    // scheme handler before falling through to the C++ built-ins
    // (github / pwsh-module).
    bpl::BlueprintLock lock;
    lock.schema = 1;
    lock.blueprint_name = spec.name;
    lock.resolved_at = gen::now_iso8601();
    for (auto& tool : spec.tools) {
        auto resolved = luban::source_resolver::resolve_with_registry(tool, sreg);
        if (!resolved) {
            std::fprintf(stderr,
                         "warning: tool `%s` skipped: %s\n",
                         tool.name.c_str(), resolved.error().c_str());
            continue;
        }
        lock.tools.emplace(tool.name, std::move(*resolved));
    }
    return lock;
}

// ---- subcommand implementations ----------------------------------------

int run_apply(const cli::ParsedArgs& args) {
    if (args.positional.empty()) {
        std::cerr << "luban blueprint apply: missing blueprint name\n";
        std::cerr << "  usage: luban blueprint apply <name>\n";
        std::cerr << "         luban blueprint apply embedded:<name>   # builtin\n";
        return 2;
    }
    std::string name = args.positional[0];

    auto resolved = resolve_blueprint(name);
    if (!resolved) {
        std::cerr << "luban blueprint apply: " << resolved.error() << "\n";
        return 1;
    }

    bool force_update = args.flags.count("update") && args.flags.at("update");

    // DESIGN §24.1 AI: every apply (TOML or Lua) constructs a Lua engine
    // and the renderer + resolver registries. TOML bps don't run any
    // register_* side effects — the engine just sits there — but the
    // apply pipeline downstream still funnels through the same registry
    // dispatch path, so `[config.git]` on a pure-TOML bp resolves the
    // same way it would on a Lua bp. Single dispatch path; no double-
    // implementation drift.
    //
    // Cost: one luaL_newstate per apply (sub-ms) for TOML bps that
    // wouldn't have needed an engine pre-AI. Worth it for the
    // architectural simplification and the §9.9 "无双码路径" promise.
    luban::lua::Engine apply_engine;
    luban::renderer_registry::RendererRegistry apply_registry;
    luban::resolver_registry::ResolverRegistry apply_resolver_registry;
    apply_engine.attach_registry(&apply_registry);
    apply_engine.attach_resolver_registry(&apply_resolver_registry);

    if (resolved->source_path.extension() == ".lua") {
        // Re-parse the Lua source inside this long-lived engine so
        // `luban.register_renderer` / `register_resolver` calls deposit
        // refs the registries can re-invoke. The first parse (in
        // resolve_blueprint above) gave us the spec; this one is purely
        // for register_* side effects. Spec from the second parse is
        // discarded — deterministic Lua parse so it's identical anyway.
        auto reparsed = luban::blueprint_lua::parse_file_in_engine(
            apply_engine, resolved->source_path);
        if (!reparsed) {
            std::cerr << "re-parse for renderer/resolver engine: "
                      << reparsed.error() << "\n";
            return 1;
        }
    }

    // Embedded blueprints don't have a sibling .lock file (no path to
    // anchor it on); fall through to fresh resolution inside resolve_lock
    // by passing an empty path. --update forces fresh resolve even when
    // an on-disk lock exists.
    auto lock = resolve_lock(resolved->source_path, resolved->spec,
                              force_update, &apply_resolver_registry);
    if (!lock) {
        std::cerr << "lock resolution: " << lock.error() << "\n";
        return 1;
    }

    // After --update on a file-backed blueprint, persist the freshly
    // resolved lock back so subsequent `bp apply` (without --update)
    // sees the new pins. Embedded blueprints have no on-disk lock so
    // we skip the write — re-running with --update each time is the
    // intended workflow there.
    if (force_update && !resolved->source_path.empty()) {
        auto lp = lock_path_for(resolved->source_path);
        if (auto w = luban::blueprint_lock::write_file(lp, *lock); !w) {
            std::cerr << "lock write " << lp.string() << ": " << w.error() << "\n";
            return 1;
        }
        std::printf("updated lock -> %s\n", lp.string().c_str());
    }

    luban::blueprint_apply::ApplyOptions opts;
    opts.dry_run = args.flags.count("dry-run") && args.flags.at("dry-run");
    // bp_source_root = parent of the blueprints/ dir that holds source_path.
    // post_install paths prefixed `bp:` resolve against this root, letting a
    // bp ship a registration script alongside its blueprint without having
    // to inject it into the upstream artifact (DESIGN §9.9).
    if (!resolved->source_path.empty()) {
        // source_path is `<root>/blueprints/<name>.toml` so two parents up
        // gives the source root. parent_path() once strips the filename;
        // parent_path() again strips the `blueprints` segment.
        auto bp_root = resolved->source_path.parent_path().parent_path();
        if (!bp_root.empty()) opts.bp_source_root = bp_root;
    }

    // Always wire the renderer registry — apply runs single-path
    // (DESIGN §24.1 AI). For TOML bps the registry is empty but apply
    // dispatch still funnels through it so the fall-through to the
    // builtin path is the same code as Lua bps' shadow-or-fallthrough.
    opts.renderer_registry = &apply_registry;

    auto result = luban::blueprint_apply::apply(resolved->spec, *lock, opts);
    if (!result) {
        std::cerr << "apply: " << result.error() << "\n";
        return 1;
    }

    std::printf("applied blueprint `%s` -> generation %d\n",
                name.c_str(), result->new_generation_id);
    std::printf("  tools fetched : %d\n", result->tools_fetched);
    std::printf("  tools external: %d\n", result->tools_external);
    std::printf("  files deployed: %d\n", result->files_deployed);
    return 0;
}

int run_unapply(const cli::ParsedArgs& args) {
    if (args.positional.empty()) {
        std::cerr << "luban blueprint unapply: missing blueprint name\n";
        return 2;
    }
    std::string raw = args.positional[0];

    // Normalize a `<source>/<bp>` qualifier to the bare bp name. `apply`
    // accepts the qualified form for source-disambiguation; the generation
    // records and `applied_blueprints` always store the bare `spec.name`
    // (spec.name comes from the blueprint TOML, not from how the user
    // typed it). Without this, `bp unapply smoke/smoke-files` would
    // match zero records and silently no-op.
    std::string name = raw;
    if (auto slash = name.find('/'); slash != std::string::npos) {
        name = name.substr(slash + 1);
    }

    auto cur_id_opt = gen::get_current();
    if (!cur_id_opt) {
        std::cerr << "no current generation; nothing to unapply\n";
        return 0;
    }
    auto cur = gen::read(*cur_id_opt);
    if (!cur) {
        std::cerr << "read current generation: " << cur.error() << "\n";
        return 1;
    }

    // Validate: refuse to mint a no-op generation if the blueprint isn't
    // actually applied. Catches typos AND qualified-form mismatches both.
    if (std::find(cur->applied_blueprints.begin(),
                  cur->applied_blueprints.end(), name) ==
        cur->applied_blueprints.end()) {
        std::cerr << "luban blueprint unapply: blueprint `" << name
                  << "` is not in the current generation\n";
        if (cur->applied_blueprints.empty()) {
            std::cerr << "  current generation has no applied blueprints\n";
        } else {
            std::cerr << "  applied:";
            for (auto& b : cur->applied_blueprints) std::cerr << " " << b;
            std::cerr << "\n";
        }
        std::cerr << "  (try `luban bp ls`)\n";
        return 1;
    }

    // Walk current's records FIRST, restore disk for entries owned by
    // the named blueprint. Each restore uses the record we have in hand,
    // so the order doesn't matter — file_deploy::restore is idempotent
    // and shim removal is just `fs::remove`. We do this BEFORE writing
    // the next generation so a crash mid-cleanup doesn't promote a
    // generation file whose disk state is partially undone.
    int files_restored = 0;
    int file_warnings  = 0;
    int shims_removed  = 0;
    int shim_warnings  = 0;

    for (auto& [path, fr] : cur->files) {
        if (fr.from_blueprint != name) continue;
        luban::file_deploy::DeployedFile d;
        d.target_path = luban::file_deploy::expand_home(fr.target_path);
        d.content_sha256 = fr.content_sha256;
        d.mode = fr.mode;
        if (fr.backup_path) d.backup_path = *fr.backup_path;
        if (auto r = luban::file_deploy::restore(d); !r) {
            std::fprintf(stderr, "warning: restore %s: %s\n",
                         path.c_str(), r.error().c_str());
            ++file_warnings;
        } else {
            ++files_restored;
        }
    }

    for (auto& [tool_name, tr] : cur->tools) {
        if (tr.from_blueprint != name) continue;
        if (tr.is_external) continue;  // luban never shimmed it
        if (!tr.shim_path.empty()) {
            if (auto r = luban::xdg_shim::remove_cmd_shim(tr.shim_path); !r) {
                std::fprintf(stderr, "warning: remove %s: %s\n",
                             tr.shim_path.c_str(), r.error().c_str());
                ++shim_warnings;
            } else {
                ++shims_removed;
            }
        }
        for (auto& sp : tr.shim_paths_secondary) {
            if (sp.empty()) continue;
            if (auto r = luban::xdg_shim::remove_cmd_shim(sp); !r) {
                std::fprintf(stderr, "warning: remove %s: %s\n",
                             sp.c_str(), r.error().c_str());
                ++shim_warnings;
            } else {
                ++shims_removed;
            }
        }
    }

    int next_id = gen::highest_id() + 1;
    gen::Generation next = *cur;
    next.id = next_id;
    next.created_at = gen::now_iso8601();

    // Drop entries owned by the named blueprint from the in-memory copy.
    auto erase_owned = [&](auto& map) {
        int removed = 0;
        for (auto it = map.begin(); it != map.end();) {
            if (it->second.from_blueprint == name) {
                it = map.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
        return removed;
    };
    int tools_dropped = erase_owned(next.tools);
    int files_dropped = erase_owned(next.files);

    auto& bps = next.applied_blueprints;
    bps.erase(std::remove(bps.begin(), bps.end(), name), bps.end());

    if (auto w = gen::write(next); !w) {
        std::cerr << "write generation: " << w.error() << "\n";
        return 1;
    }
    if (auto s = gen::set_current(next_id); !s) {
        std::cerr << "set current: " << s.error() << "\n";
        return 1;
    }
    std::printf("unapplied blueprint `%s` -> generation %d\n",
                name.c_str(), next_id);
    std::printf("  tools dropped  : %d\n", tools_dropped);
    std::printf("  files dropped  : %d\n", files_dropped);
    std::printf("  files restored : %d\n", files_restored);
    std::printf("  shims removed  : %d\n", shims_removed);
    if (file_warnings || shim_warnings) {
        std::printf("  warnings       : %d\n", file_warnings + shim_warnings);
    }
    return 0;
}

int run_ls(const cli::ParsedArgs&) {
    // User-authored blueprints in <config>/blueprints/.
    std::printf("user blueprints (%s):\n", blueprints_dir().string().c_str());
    std::error_code ec;
    auto dir = blueprints_dir();
    if (fs::is_directory(dir, ec)) {
        bool any = false;
        for (auto& e : fs::directory_iterator(dir, ec)) {
            if (!e.is_regular_file(ec)) continue;
            auto ext = e.path().extension().string();
            if (ext != ".lua" && ext != ".toml") continue;
            std::printf("  %s\n", e.path().filename().string().c_str());
            any = true;
        }
        if (!any) std::printf("  (no user blueprints yet)\n");
    } else {
        std::printf("  (directory does not exist)\n");
    }

    std::printf("\ncurrent generation: ");
    if (auto cur_id = gen::get_current()) {
        std::printf("%d\n", *cur_id);
        if (auto cur = gen::read(*cur_id)) {
            std::printf("  applied blueprints: ");
            if (cur->applied_blueprints.empty()) {
                std::printf("(none)\n");
            } else {
                bool first = true;
                for (auto& bp_name : cur->applied_blueprints) {
                    std::printf("%s%s", first ? "" : ", ", bp_name.c_str());
                    first = false;
                }
                std::printf("\n");
            }
            std::printf("  tools: %zu  files: %zu\n",
                        cur->tools.size(), cur->files.size());
        }
    } else {
        std::printf("(none)\n");
    }
    return 0;
}

int run_status(const cli::ParsedArgs&) {
    auto cur_id_opt = gen::get_current();
    if (!cur_id_opt) {
        std::printf("no current generation\n");
        return 0;
    }
    auto cur = gen::read(*cur_id_opt);
    if (!cur) {
        std::cerr << "read current: " << cur.error() << "\n";
        return 1;
    }
    std::printf("generation %d (%s)\n", cur->id, cur->created_at.c_str());
    std::printf("applied: ");
    bool first = true;
    for (auto& b : cur->applied_blueprints) {
        std::printf("%s%s", first ? "" : ", ", b.c_str());
        first = false;
    }
    if (cur->applied_blueprints.empty()) std::printf("(none)");
    std::printf("\n\ntools (%zu):\n", cur->tools.size());
    for (auto& [name, t] : cur->tools) {
        if (t.is_external) {
            std::printf("  %s [external] %s\n", name.c_str(), t.external_path.c_str());
        } else {
            std::printf("  %s -> %s\n", name.c_str(), t.shim_path.c_str());
        }
    }
    std::printf("\nfiles (%zu):\n", cur->files.size());
    for (auto& [path, f] : cur->files) {
        const char* mode_str = "?";
        switch (f.mode) {
            case bp::FileMode::Replace: mode_str = "replace"; break;
            case bp::FileMode::DropIn:  mode_str = "drop-in"; break;
            case bp::FileMode::Merge:   mode_str = "merge";   break;
            case bp::FileMode::Append:  mode_str = "append";  break;
        }
        std::printf("  %s [%s] from %s\n", path.c_str(),
                    mode_str, f.from_blueprint.c_str());
    }
    return 0;
}

int run_rollback(const cli::ParsedArgs& args) {
    auto cur_id_opt = gen::get_current();
    if (!cur_id_opt) {
        std::cerr << "no current generation; nothing to rollback\n";
        return 1;
    }
    int target_id;
    if (!args.positional.empty()) {
        try {
            target_id = std::stoi(args.positional[0]);
        } catch (...) {
            std::cerr << "rollback: argument must be an integer generation id\n";
            return 2;
        }
    } else {
        // Default: previous generation.
        auto ids = gen::list_ids();
        // Find the largest id < current.
        target_id = -1;
        for (auto id : ids) {
            if (id < *cur_id_opt && id > target_id) target_id = id;
        }
        if (target_id < 0) {
            std::cerr << "no prior generation to rollback to\n";
            return 1;
        }
    }
    auto target = gen::read(target_id);
    if (!target) {
        std::cerr << "rollback: " << target.error() << "\n";
        return 1;
    }

    // Reconcile on-disk state to the target generation BEFORE flipping
    // the current pointer. If reconcile errors hard (rare — best-effort
    // mode pushes per-file/shim issues into warnings, not the result),
    // current stays untouched so the system isn't half-rolled-back.
    auto rec = luban::blueprint_reconcile::reconcile_to(target_id);
    if (!rec) {
        std::cerr << "rollback reconcile: " << rec.error() << "\n";
        return 1;
    }
    if (auto s = gen::set_current(target_id); !s) {
        std::cerr << "rollback set current: " << s.error() << "\n";
        return 1;
    }
    std::printf("rolled back to generation %d\n", target_id);
    std::printf("  files restored  : %d\n", rec->files_restored);
    std::printf("  files recreated : %d\n", rec->files_recreated);
    std::printf("  shims removed   : %d\n", rec->shims_removed);
    std::printf("  shims recreated : %d\n", rec->shims_recreated);
    for (auto& w : rec->warnings) {
        std::fprintf(stderr, "warning: %s\n", w.c_str());
    }
    return 0;
}

// ---- dispatch ---------------------------------------------------------

int run_gc(const cli::ParsedArgs&) {
    // DESIGN §16 declares `bp gc` as a v1.0 subcommand for cleaning
    // blueprint-side garbage (旧 generations / 无引用 store 项 / 过期
    // backup). Implementation deferred to v1.x — generation lifetime
    // accounting + store ref-counting + backup expiry are independent
    // pieces, each non-trivial. Until then, surface the gap explicitly
    // so users discover the verb but aren't surprised by partial behavior.
    std::fprintf(stderr,
                 "luban bp gc: not yet implemented (v1.x).\n"
                 "  Manual cleanup until then:\n"
                 "    luban clean --cache              free luban download cache\n"
                 "    rm -r <state>/luban/generations/<old>.json   prune old generations\n");
    return 1;
}

int run_blueprint(const cli::ParsedArgs& args) {
    if (args.positional.empty()) {
        std::cerr << "luban blueprint: missing subcommand\n";
        std::cerr << "  available: apply | unapply | ls | status | rollback | gc | source | search\n";
        std::cerr << "  short alias: luban bp <subcommand>\n";
        return 2;
    }
    std::string sub = args.positional[0];
    cli::ParsedArgs rest = args;
    rest.positional.erase(rest.positional.begin());

    if (sub == "apply")    return run_apply(rest);
    if (sub == "unapply")  return run_unapply(rest);
    if (sub == "ls")       return run_ls(rest);
    if (sub == "status")   return run_status(rest);
    if (sub == "rollback") return run_rollback(rest);
    if (sub == "gc")       return run_gc(rest);
    if (sub == "source" || sub == "src") return run_bp_source(rest);
    if (sub == "search")                 return run_bp_search(rest);

    std::cerr << "luban blueprint: unknown subcommand `" << sub << "`\n";
    std::cerr << "  available: apply | unapply | ls | status | rollback | gc | source (alias: src) | search\n";
    return 2;
}

}  // namespace

void register_blueprint() {
    cli::Subcommand c;
    c.name = "blueprint";
    c.help = "manage blueprints (apply / unapply / ls / status / rollback)";
    c.group = "project";
    c.long_help =
        "  `luban blueprint` (alias `luban bp`) drives the v1.0 layer model:\n"
        "  apply a blueprint to install tools + render configs into XDG\n"
        "  paths, then snapshot what we did into a generation file under\n"
        "  <state>/generations/.\n\n"
        "  Blueprint sources live at <config>/luban/blueprints/<name>.{lua,toml}.\n"
        "  Lockfiles are <name>.{lua,toml}.lock; commit them to git.\n\n"
        "  Subcommands:\n"
        "    apply <name>     fetch + render + deploy + new generation\n"
        "    unapply <name>   drop a blueprint's contributions, new generation\n"
        "    ls               list known blueprints + active generation\n"
        "    status           show current generation contents\n"
        "    rollback [N]     flip current to generation N (default: prev)\n"
        "    gc               (v1.x) prune old generations + unreferenced store\n"
        "    source <op>      manage bp sources: add | rm | ls | update\n"
        "                     short alias: `bp src <op>`\n"
        "    search [<pat>]   search blueprints across sources + user-local\n\n"
        "  Flags:\n"
        "    --dry-run        log what would happen without changing disk\n"
        "    --update         (apply only) re-resolve lock from spec instead\n"
        "                     of reading the existing .lock file; on file-backed\n"
        "                     blueprints the new lock is written back\n"
        "    --yes            (source add) skip the trust prompt\n"
        "  Options:\n"
        "    --ref <ref>      (source add/update) branch / tag / sha (default: main)\n"
        "    --name <name>    (source add) override default source name\n"
        "                     (default: derived from URL — repo basename / dir basename)";
    c.flags = {"dry-run", "update", "yes"};
    c.opts = {{"ref", ""}, {"name", ""}};
    c.forward_rest = false;
    c.examples = {
        "luban blueprint apply main/cli-base\tApply the cli-base blueprint from main",
        "luban bp ls\tShort alias; list blueprints",
        "luban blueprint rollback\tRevert to the previous generation",
    };
    c.run = run_blueprint;

    // Short alias `bp` shares the same handler + flags + long_help. Copy
    // BEFORE std::move(c) so the alias inherits a fully-populated
    // Subcommand — copying after the move silently dropped flags,
    // long_help, examples (moved-from std::vector / std::string is
    // implementation-defined empty), which made `bp --dry-run`
    // surface "unknown option" while `blueprint --dry-run` worked.
    cli::Subcommand alias = c;
    alias.name = "bp";
    alias.help = "alias for `luban blueprint`";

    cli::register_subcommand(std::move(c));
    cli::register_subcommand(std::move(alias));
}

}  // namespace luban::commands
