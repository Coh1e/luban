// `luban blueprint` — verb family for applying / listing remote
// blueprints (DESIGN §5).
//
// Subcommands:
//   apply <name>      — fetch tools + render configs + deploy files
//   list              — list user-local + per-source available bps
//   source <op>       — add | update bp source registry entries
//
// Per DESIGN §11 the following are NOT in MVP and have been removed:
//   unapply / status / rollback / gc — generation/rollback infra dropped
//   source rm / source ls — strict §5 set is add + update
//   search — `list` does cross-source enumeration
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
#include "../iso_time.hpp"
#include "../cli.hpp"
#include "../file_deploy.hpp"
#include "../log.hpp"
#include "../lua_engine.hpp"
#include "../lua_frontend.hpp"
#include "../paths.hpp"
#include "../renderer_registry.hpp"
#include "../resolver_registry.hpp"
#include "../source_registry.hpp"
#include "../source_resolver.hpp"
#include "../xdg_shim.hpp"
#include "bp_source.hpp"

#include "luban/embedded_configs/git.hpp"
#include "luban/embedded_configs/bat.hpp"
#include "luban/embedded_configs/fastfetch.hpp"
#include "luban/embedded_configs/yazi.hpp"
#include "luban/embedded_configs/delta.hpp"

namespace luban::commands {

namespace {

namespace fs = std::filesystem;
namespace bp = luban::blueprint;
namespace bpl = luban::blueprint_lock;

// Builtin renderer table — kept here (rather than in lua_frontend or
// config_renderer) because pre-loading is the orchestration's job, and
// config_renderer is now Lua-free (DESIGN §24.1 AH). To add a new
// builtin: drop a new templates/configs/<X>.lua, append it to the
// CMakeLists.txt embed_text foreach, #include the header above, and
// add a row here.
struct BuiltinRenderer {
    const char* name;
    const char* embedded_source;
};
constexpr BuiltinRenderer kBuiltinRenderers[] = {
    {"git",       luban::embedded_configs::git_lua},
    {"bat",       luban::embedded_configs::bat_lua},
    {"fastfetch", luban::embedded_configs::fastfetch_lua},
    {"yazi",      luban::embedded_configs::yazi_lua},
    {"delta",     luban::embedded_configs::delta_lua},
};

// Pre-load the 5 builtin renderers (and any user override at
// <config>/luban/configs/<X>.lua) into `reg` via lua_frontend. Bps that
// later call luban.register_renderer can shadow these last-wins.
//
// Returns the first error encountered; subsequent builtins are skipped
// (that builtin won't be in the registry, and the apply will fail with
// "no renderer for ..." when it hits the corresponding [config.X]).
std::expected<void, std::string> preload_builtin_renderers(
    luban::lua::Engine& engine,
    luban::renderer_registry::RendererRegistry& reg) {
    auto user_override_dir = paths::config_dir() / "configs";
    for (const auto& b : kBuiltinRenderers) {
        std::string source;
        std::string chunkname;
        fs::path user_path = user_override_dir / (std::string(b.name) + ".lua");
        std::error_code ec;
        if (fs::is_regular_file(user_path, ec)) {
            std::ifstream in(user_path);
            if (!in) {
                return std::unexpected("cannot read user override " +
                                       user_path.string());
            }
            std::ostringstream ss;
            ss << in.rdbuf();
            source = ss.str();
            chunkname = "@" + user_path.string();
        } else {
            source = b.embedded_source;
            chunkname = "=embedded:" + std::string(b.name);
        }
        auto fns = luban::lua_frontend::wrap_embedded_module(
            engine.state(), source, chunkname);
        if (!fns) {
            return std::unexpected("preload `" + std::string(b.name) +
                                   "`: " + fns.error());
        }
        reg.register_native(b.name, std::move(*fns));
    }
    return {};
}

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

/// Locate a `<bp-name>.lua` inside the on-disk dir of a registered
/// source. Returns empty path if no match.
fs::path find_blueprint_in_source_dir(const fs::path& source_root,
                                      std::string_view name) {
    auto base = source_root / "blueprints";
    std::error_code ec;
    fs::path lua_path = base / (std::string(name) + ".lua");
    if (fs::is_regular_file(lua_path, ec)) return lua_path;
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
    if (ext == ".lua") return luban::blueprint_lua::parse_file(src);
    return std::unexpected("unsupported blueprint extension `" + ext +
                           "` (only `.lua` is accepted; DESIGN §2 #6)");
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
    lock.resolved_at = luban::iso_time::now();
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

    // Phase 6 (DESIGN §24.1 AI / §9.9 line 656): pre-load the 5 builtin
    // renderers + any user override into the registry BEFORE the bp's
    // parse runs. Bp's register_renderer can then shadow them last-wins.
    if (auto pl = preload_builtin_renderers(apply_engine, apply_registry); !pl) {
        std::cerr << "preload builtins: " << pl.error() << "\n";
        return 1;
    }

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
    opts.yes     = args.flags.count("yes")     && args.flags.at("yes");
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

    // DESIGN §8 trust model: source officiality flows from the bp source
    // registry into the apply-time trust summary. Bare-name / user-local
    // bps (no registered source) default to non-official — local bps
    // already needed an explicit `bp source add` for everything except
    // ad-hoc development under <config>/luban/blueprints/, so requiring
    // confirmation there errs on the safe side.
    opts.bp_source_name = resolved->bp_source;
    if (!resolved->bp_source.empty()) {
        if (auto entries = sr::read(); entries) {
            if (auto e = sr::find(*entries, resolved->bp_source)) {
                opts.source_official = e->official;
            } else {
                opts.source_official = false;
            }
        } else {
            opts.source_official = false;
        }
    } else {
        // No registered source = either user-local under <config>/blueprints/
        // (treat as non-official; DESIGN §8 only the official allowlist
        // confers default trust) or embedded:* (rejected earlier).
        opts.source_official = false;
        if (opts.bp_source_name.empty()) opts.bp_source_name = "user-local";
    }

    auto result = luban::blueprint_apply::apply(resolved->spec, *lock, opts);
    if (!result) {
        std::cerr << "apply: " << result.error() << "\n";
        return 1;
    }

    std::printf("applied blueprint `%s`\n", name.c_str());
    std::printf("  tools fetched : %d\n", result->tools_fetched);
    std::printf("  tools external: %d\n", result->tools_external);
    std::printf("  files deployed: %d\n", result->files_deployed);
    return 0;
}

// ---- dispatch ---------------------------------------------------------

int run_blueprint(const cli::ParsedArgs& args) {
    if (args.positional.empty()) {
        std::cerr << "luban blueprint: missing subcommand\n";
        std::cerr << "  available: apply | list | source\n";
        std::cerr << "  short alias: luban bp <subcommand>\n";
        return 2;
    }
    std::string sub = args.positional[0];
    cli::ParsedArgs rest = args;
    rest.positional.erase(rest.positional.begin());

    if (sub == "apply")    return run_apply(rest);
    if (sub == "list")     return run_bp_list(rest);
    if (sub == "ls")       return run_bp_list(rest);  // soft alias
    if (sub == "source" || sub == "src") return run_bp_source(rest);

    std::cerr << "luban blueprint: unknown subcommand `" << sub << "`\n";
    std::cerr << "  available: apply | list | source (alias: src)\n";
    return 2;
}

}  // namespace

void register_blueprint() {
    cli::Subcommand c;
    c.name = "blueprint";
    c.help = "manage blueprints (apply / list / source)";
    c.group = "project";
    c.long_help =
        "  `luban blueprint` (alias `luban bp`) drives the bp layer model:\n"
        "  apply a blueprint to install tools + render configs into XDG\n"
        "  paths.\n\n"
        "  Blueprint sources live at <config>/luban/blueprints/<name>.lua,\n"
        "  or in registered remote bp sources (`bp source add`).\n\n"
        "  Subcommands:\n"
        "    apply <name>     fetch + render + deploy\n"
        "    list             list user-local + per-source available bps\n"
        "    source <op>      manage bp sources: add | update\n"
        "                     short alias: `bp src <op>`\n\n"
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
        "luban bp source add Coh1e/luban-bps --name main\tRegister a remote bp source",
        "luban bp apply main/bootstrap\tApply the foundation bp",
        "luban bp list\tList user-local + source-available bps",
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
