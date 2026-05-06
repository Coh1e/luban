// See `blueprint_apply.hpp`.
//
// The orchestrator is mostly mechanical wiring. Most of the per-step
// logic was already paid for in S3-S5; this file's job is just to call
// the right thing in the right order and feed each step's output into
// the next.
//
// Notable v1 simplifications:
//
// 1. Shim writing at ~/.local/bin/ is delegated to src/xdg_shim.cpp so
//    blueprint_reconcile (rollback) can rebuild shims it removed. The
//    .cmd format matches src/shim.cpp's v0.x text shim
//    (`@echo off / "<exe>" %*`); the legacy v0.x shim writer still
//    targets `<data>/bin/` for the scoop/component pipeline — both
//    coexist until v0.x is retired.
//
// 2. Program renderers always deploy in drop-in mode. The renderer
//    decides the target_path; we treat its output like the user wrote
//    a [file."<path>"] entry with mode = drop-in. Down the road
//    renderers might want to declare mode themselves, but for v1 every
//    built-in is drop-in.
//
// 3. apply() always writes generation N+1 even when we hit an error
//    mid-flight? No — we DON'T. We assemble the in-memory Generation
//    incrementally and only persist it at the very end. That way a
//    crashed apply leaves no record, and a re-run sees the previous
//    successful generation as authoritative.

#include "blueprint_apply.hpp"

#include <fstream>
#include <set>
#include <sstream>
#include <system_error>

#include "external_skip.hpp"
#include "file_deploy.hpp"
#include "generation.hpp"
#include "log.hpp"
#include "paths.hpp"
#include "platform.hpp"
#include "proc.hpp"
#include "config_renderer.hpp"
#include "source_resolver.hpp"
#include "store.hpp"
#include "xdg_shim.hpp"

namespace luban::blueprint_apply {

namespace {

namespace fs = std::filesystem;
namespace bp = luban::blueprint;
namespace bpl = luban::blueprint_lock;
namespace gen = luban::generation;

/// Resolve a post_install script path to an absolute path with a
/// traversal guard.
///
/// Two roots possible:
///   - default (no prefix): path is artifact-relative — script lives
///     inside the extracted upstream archive (vcpkg's bootstrap-vcpkg.bat
///     is the canonical case). Guard against escaping store_dir.
///   - `bp:` prefix:        path is bp-source-relative — script lives in
///     the bp source repo alongside the .toml/.lua blueprint file
///     (Maple Mono font registration is the canonical case — upstream
///     ships only .ttf files, the registration logic has to come from
///     the bp). Guard against escaping bp_source_root. Errors loudly if
///     bp_source_root is unset (programmatic callers without context).
///
/// In either case the resolved path must exist as a regular file
/// (rejects typos and missing scripts).
std::expected<fs::path, std::string> resolve_post_install(
    const fs::path& store_dir, std::string_view rel,
    const std::optional<fs::path>& bp_source_root) {
    constexpr std::string_view kBpPrefix = "bp:";

    fs::path root;
    fs::path joined;
    std::string display_rel(rel);

    if (rel.starts_with(kBpPrefix)) {
        if (!bp_source_root.has_value()) {
            return std::unexpected(
                "post_install `" + display_rel + "` uses `bp:` prefix but "
                "bp_source_root is unset (only programmatic apply callers "
                "hit this path; bp src apply sets it automatically)");
        }
        std::string_view tail = rel.substr(kBpPrefix.size());
        root = *bp_source_root;
        joined = root / fs::path(std::string(tail));
    } else {
        root = store_dir;
        joined = root / fs::path(std::string(rel));
    }

    fs::path normalized = joined.lexically_normal();

    // Require the normalized path to be lexically inside `root`.
    // lexically_relative returns something starting with ".." if it
    // escapes; guard that. Use string() rather than native() so the
    // comparison is portable across Windows wide vs POSIX narrow paths.
    fs::path probe = normalized.lexically_relative(root);
    std::string probe_s = probe.string();
    if (probe_s.empty() || probe_s.rfind("..", 0) == 0) {
        return std::unexpected("post_install path escapes its root: " +
                               display_rel);
    }

    std::error_code ec;
    if (!fs::is_regular_file(normalized, ec)) {
        return std::unexpected("post_install script not found: " +
                               normalized.string());
    }
    return normalized;
}

/// Run a post_install script with cwd at the artifact root. Wraps with
/// the OS shell (cmd /c on Windows, /bin/sh on POSIX) so .bat/.cmd/.sh
/// all work without per-extension dispatch. Returns the script's exit
/// code (0 = success).
std::expected<void, std::string> run_post_install(
    const fs::path& script, const fs::path& cwd) {
    std::vector<std::string> cmd;
#ifdef _WIN32
    cmd = {"cmd.exe", "/c", script.string()};
#else
    cmd = {"/bin/sh", script.string()};
#endif
    log::stepf("post_install: {}", script.string());
    int rc = proc::run(cmd, cwd.string(), {});
    if (rc != 0) {
        return std::unexpected("post_install exited with code " +
                               std::to_string(rc));
    }
    return {};
}

/// Pick the right LockedPlatform for the current host. For v1 we just
/// match host_triplet() exactly — "windows-x64" maps to the
/// "windows-x64" key in the lock. No fallback chain (e.g. windows-x64
/// → windows-* → any) yet; that complexity buys little until we have
/// real-world cases that need it.
const bpl::LockedPlatform* pick_platform(const bpl::LockedTool& t) {
    auto triplet = luban::platform::host_triplet();
    auto it = t.platforms.find(triplet);
    if (it == t.platforms.end()) return nullptr;
    return &it->second;
}

/// Build a generation by combining the previous current generation
/// (if any) with the new state from this apply. Re-applying a
/// blueprint replaces its previous tools/files entries; we don't
/// merge per-tool because that gets confusing fast.
gen::Generation make_next_generation(int next_id,
                                     std::string_view blueprint_name) {
    gen::Generation g;
    g.schema = 1;
    g.id = next_id;
    g.created_at = gen::now_iso8601();

    // Inherit from current (if any) so re-applying one blueprint
    // doesn't drop everything else.
    if (auto cur_id = gen::get_current()) {
        if (auto cur = gen::read(*cur_id)) {
            g.applied_blueprints = cur->applied_blueprints;
            g.tools = cur->tools;
            g.files = cur->files;
        }
    }

    // Drop entries the named blueprint previously contributed; the
    // caller's loop adds the fresh ones back.
    auto drop_from_bp = [&](auto& map) {
        for (auto it = map.begin(); it != map.end();) {
            if (it->second.from_blueprint == blueprint_name) {
                it = map.erase(it);
            } else {
                ++it;
            }
        }
    };
    drop_from_bp(g.tools);
    drop_from_bp(g.files);

    // Ensure the blueprint name appears in applied_blueprints exactly
    // once.
    auto& bps = g.applied_blueprints;
    bps.erase(std::remove(bps.begin(), bps.end(), std::string(blueprint_name)),
              bps.end());
    bps.emplace_back(blueprint_name);
    return g;
}

}  // namespace

std::expected<ApplyResult, std::string> apply(const bp::BlueprintSpec& spec,
                                              const bpl::BlueprintLock& lock,
                                              const ApplyOptions& opts) {
    ApplyResult result;
    int next_id = gen::highest_id() + 1;
    result.new_generation_id = next_id;

    // ---- Preflight: meta.requires gating (v0.2.0, 议题 M (b) followup) ---
    //
    // Each name in spec.meta.requires_ must already appear in the current
    // generation's applied_blueprints. Hard error otherwise — listing every
    // missing dep + the exact `luban bp apply` line to fix it. We do NOT
    // auto-recurse into deps; explicit beats clever for the layered
    // foundation/cpp-toolchain story (see DESIGN line 1131-1132).
    //
    // No current generation = treat as empty applied set, which fails
    // every requires entry but with the right error message.
    //
    // Qualifier handling (v0.2.4): apply() only sees `spec.name` which is
    // the bare bp name (e.g. "foundation"); commands/blueprint.cpp resolves
    // `main/foundation` to a spec but doesn't currently propagate the
    // source qualifier into the generation record. Meanwhile bp authors
    // (sensibly) write `requires = ["main/foundation"]`. Result: applied
    // set holds "foundation" but requires says "main/foundation" — false
    // negative, gating blocks valid applies. Until qualified names land
    // in generation records, compare bare-name vs bare-name on both sides.
    // Trade-off: cross-source name collision (`other/foundation` and
    // `main/foundation` both satisfy each other) — accepted for the v0.2.x
    // window since it's a strictly weaker check, and bp source repo names
    // are user-controlled (no malicious-source story to worry about yet).
    auto strip_source = [](std::string_view s) -> std::string_view {
        auto slash = s.find('/');
        return (slash != std::string_view::npos) ? s.substr(slash + 1) : s;
    };
    if (!spec.meta.requires_.empty()) {
        std::set<std::string> applied;
        if (auto current_id = gen::get_current(); current_id) {
            if (auto cur = gen::read(*current_id); cur) {
                for (auto& name : cur->applied_blueprints) {
                    applied.insert(std::string(strip_source(name)));
                }
            }
            // Read failure is treated as "no applied bps known" — same
            // outcome as no current generation. The user gets a clear
            // requires error rather than a cryptic IO failure.
        }
        std::vector<std::string> missing;
        for (auto& req : spec.meta.requires_) {
            if (!applied.contains(std::string(strip_source(req)))) {
                missing.push_back(req);
            }
        }
        if (!missing.empty()) {
            std::string msg = "blueprint `" + spec.name +
                              "` requires these unapplied blueprint(s):\n";
            for (auto& m : missing) {
                msg += "  - " + m + "\n      apply with:  luban bp apply " + m + "\n";
            }
            return std::unexpected(msg);
        }
    }

    auto next_gen = make_next_generation(next_id, spec.name);

    // ---- Tools ---------------------------------------------------------
    const size_t total_tools = spec.tools.size();
    size_t tool_idx = 0;
    for (auto& tool : spec.tools) {
        ++tool_idx;
        log::stepf("[{}/{}] tool: {}", tool_idx, total_tools, tool.name);

        // Step 1: external skip. The probe target defaults to `tool.name`
        // (e.g. "cmake" finds /usr/bin/cmake on a system that already has
        // it). When the brand and the canonical PATH binary differ —
        // openssh ↔ ssh.exe is the load-bearing case — the blueprint
        // sets `external_skip` to override the probe target.
        std::string probe_target = tool.external_skip.value_or(tool.name);
        if (auto ext = luban::external_skip::probe(probe_target)) {
            gen::ToolRecord rec;
            rec.from_blueprint = spec.name;
            rec.is_external = true;
            rec.external_path = ext->resolved_path.string();
            next_gen.tools[tool.name] = std::move(rec);
            ++result.tools_external;
            log::infof("  external (provided by {})", ext->resolved_path.string());
            continue;
        }

        // Step 2: lookup in lock.
        // Soft-skip when a tool has no lock entry: the lock resolver
        // already warned about why (e.g. "github: not yet implemented");
        // continuing lets [programs] / [files] still apply for the
        // tools that ARE locked. Future S8 work will tighten this when
        // the github resolver makes "no lock" rare.
        auto lock_it = lock.tools.find(tool.name);
        if (lock_it == lock.tools.end()) {
            log::warnf("tool `{}` skipped (no lock entry; see prior warnings)",
                       tool.name);
            continue;
        }
        auto* plat = pick_platform(lock_it->second);
        if (!plat) {
            log::warnf("tool `{}` skipped (no platform `{}` in lock)",
                       tool.name, luban::platform::host_triplet());
            continue;
        }

        // Step 3: fetch into store (idempotent).
        if (opts.dry_run) {
            log::infof("  (dry-run) would fetch {}", plat->artifact_id);
            ++result.tools_fetched;
            continue;
        }
        log::infof("  fetching {} ({})", plat->artifact_id,
                   luban::platform::host_triplet());
        auto fetched = luban::store::fetch(plat->artifact_id, plat->url,
                                           plat->sha256, plat->bin);
        if (!fetched) {
            return std::unexpected("fetch " + tool.name + ": " + fetched.error());
        }

        // Step 3.5: post_install hook (DESIGN §9.9). Skip on cache hits —
        // post_install is part of "install" and content-addressed
        // artifacts are immutable, so re-running on the same artifact_id
        // would re-bootstrap an already-bootstrapped tool (vcpkg's
        // canonical case). Fresh extraction always runs it.
        if (tool.post_install && !fetched->was_already_present) {
            log::infof("  post_install: {}", *tool.post_install);
            auto script = resolve_post_install(fetched->store_dir,
                                               *tool.post_install,
                                               opts.bp_source_root);
            if (!script) {
                return std::unexpected("post_install " + tool.name + ": " +
                                       script.error());
            }
            if (auto r = run_post_install(*script, fetched->store_dir); !r) {
                return std::unexpected("post_install " + tool.name + ": " +
                                       r.error());
            }
        }

        // Step 4: shim(s) under ~/.local/bin/.
        //
        // Single-shim path (legacy / 99% of tools): use the basename of
        // `plat->bin` as alias (typical case: "rg.exe" → alias "rg").
        //
        // Multi-shim path (tool.shims non-empty): each entry is treated
        // as a path relative to the artifact root, mirroring `bin`
        // semantics. Same path-traversal guard as resolve_post_install
        // so a malicious blueprint can't shim arbitrary files. The first
        // entry is the primary shim (recorded in shim_path / bin_path_rel);
        // any additional entries land in shim_paths_secondary alongside
        // their bin_paths_rel_secondary so blueprint_reconcile can
        // delete or recreate every shim a tool installed.
        //
        // no_shim = true bypasses this entirely — used for "tools" that
        // register themselves through non-PATH channels (fonts via
        // HKCU\...\Fonts + AddFontResourceEx, etc).
        std::string primary_shim;
        std::string primary_bin_rel;
        std::vector<std::string> secondary_shims;
        std::vector<std::string> secondary_bin_rels;
        std::set<std::string> aliases_seen;

        if (tool.no_shim) {
            // Record the tool as fetched-but-not-shimmed so generation
            // tracking stays consistent with non-shim tools. shim_path /
            // bin_path_rel stay empty — rollback's reconcile path treats
            // empty shim_path as "no .cmd to delete" and uses
            // artifact_id + store::path_for() to locate the store entry.
            gen::ToolRecord rec;
            rec.from_blueprint = spec.name;
            rec.is_external = false;
            rec.artifact_id = plat->artifact_id;
            next_gen.tools[tool.name] = std::move(rec);
            ++result.tools_fetched;
            log::infof("  no_shim — registration handled by post_install");
            continue;  // skip the rest of Step 4 + the trailing record-write
        }

        // Single helper that handles one (absolute path inside store_dir)
        // shim target — used by both the explicit `shims` list and the
        // `shim_dir` enumeration below. Returns a string error on
        // path-traversal, missing file, or empty alias; returns ok
        // silently when the alias was already written (dedup).
        auto add_shim = [&](const fs::path& absolute,
                            const std::string& rel_str)
            -> std::expected<void, std::string> {
            fs::path normalized = absolute.lexically_normal();
            fs::path probe = normalized.lexically_relative(fetched->store_dir);
            std::string probe_s = probe.string();
            if (probe_s.empty() || probe_s.rfind("..", 0) == 0) {
                return std::unexpected("shim " + tool.name +
                                       ": entry escapes artifact root: " + rel_str);
            }
            std::error_code ec_f;
            if (!fs::is_regular_file(normalized, ec_f)) {
                return std::unexpected("shim " + tool.name +
                                       ": target not found: " + normalized.string());
            }
            std::string alias = normalized.stem().string();
            if (alias.empty()) {
                return std::unexpected("shim " + tool.name +
                                       ": cannot derive alias from `" + rel_str + "`");
            }
            if (!aliases_seen.insert(alias).second) return {};  // dedup
            auto shim = luban::xdg_shim::write_cmd_shim(alias, normalized);
            if (!shim) {
                return std::unexpected("shim " + tool.name + " (" + alias +
                                       "): " + shim.error());
            }
            if (primary_shim.empty()) {
                primary_shim = shim->string();
                primary_bin_rel = probe.generic_string();
            } else {
                secondary_shims.push_back(shim->string());
                secondary_bin_rels.push_back(probe.generic_string());
            }
            return {};
        };

        if (tool.shims.empty() && !tool.shim_dir) {
            // Default: one shim derived from plat->bin.
            std::string alias = fs::path(plat->bin).stem().string();
            if (alias.empty()) alias = tool.name;
            auto shim = luban::xdg_shim::write_cmd_shim(alias, fetched->bin_path);
            if (!shim) {
                return std::unexpected("shim " + tool.name + ": " + shim.error());
            }
            log::infof("  shim: {} -> {}", alias, paths::xdg_bin_home().string());
            primary_shim = shim->string();
            std::error_code rel_ec;
            auto rel = fs::relative(fetched->bin_path, fetched->store_dir, rel_ec);
            if (!rel_ec) primary_bin_rel = rel.generic_string();
        } else {
            // Explicit `shims` entries first — they get priority and any
            // alias in this list is locked in before shim_dir expansion
            // runs (so a curated entry wins over an auto-discovered one
            // with the same alias).
            for (auto const& rel : tool.shims) {
                if (auto r = add_shim(fetched->store_dir / fs::path(rel), rel); !r) {
                    return std::unexpected(r.error());
                }
            }
            // shim_dir: enumerate every .exe under <store_dir>/<shim_dir>/.
            // Non-windows hosts skip the .exe filter and use the
            // executable bit instead — `luban-bps` only sets shim_dir on
            // Windows-shipped tools today, so this branch is dormant
            // until POSIX bps land.
            if (tool.shim_dir) {
                fs::path sd_abs = fetched->store_dir / fs::path(*tool.shim_dir);
                std::error_code ec_iter;
                if (!fs::is_directory(sd_abs, ec_iter)) {
                    return std::unexpected("shim " + tool.name + ": shim_dir `" +
                                           *tool.shim_dir + "` is not a directory");
                }
                for (auto const& e : fs::directory_iterator(sd_abs, ec_iter)) {
                    if (!e.is_regular_file(ec_iter)) continue;
                    auto p = e.path();
#ifdef _WIN32
                    if (p.extension() != ".exe") continue;
#else
                    auto perms = fs::status(p, ec_iter).permissions();
                    if ((perms & fs::perms::owner_exec) == fs::perms::none) continue;
#endif
                    std::string rel_str = (*tool.shim_dir + "/" + p.filename().string());
                    if (auto r = add_shim(p, rel_str); !r) {
                        return std::unexpected(r.error());
                    }
                }
            }
            log::infof("  shims: {} alias(es) -> {}",
                       aliases_seen.size(), paths::xdg_bin_home().string());
        }

        gen::ToolRecord rec;
        rec.from_blueprint = spec.name;
        rec.artifact_id = plat->artifact_id;
        rec.shim_path = primary_shim;
        rec.bin_path_rel = primary_bin_rel;
        rec.shim_paths_secondary = std::move(secondary_shims);
        rec.bin_paths_rel_secondary = std::move(secondary_bin_rels);
        rec.is_external = false;
        next_gen.tools[tool.name] = std::move(rec);
        ++result.tools_fetched;
    }

    // ---- Configs ------------------------------------------------------
    luban::config_renderer::Context ctx;
    ctx.home = paths::home();
    ctx.xdg_config = paths::config_dir();
    ctx.blueprint_name = spec.name;
    ctx.platform = std::string(luban::platform::host_os());

    const size_t total_cfgs = spec.configs.size();
    size_t cfg_idx = 0;
    for (auto& cfg : spec.configs) {
        ++cfg_idx;
        // for_tool overrides which renderer handles this cfg block. Falls
        // back to cfg.name (the conventional same-name binding) when unset.
        const std::string& renderer_name = cfg.for_tool.value_or(cfg.name);
        log::stepf("[{}/{}] config: {}", cfg_idx, total_cfgs, cfg.name);
        auto rendered = luban::config_renderer::render(renderer_name, cfg.config, ctx);
        if (!rendered) {
            return std::unexpected("render " + cfg.name + ": " + rendered.error());
        }
        if (opts.dry_run) {
            log::infof("  (dry-run) would render -> {}",
                       rendered->target_path.string());
            ++result.files_deployed;
            continue;
        }

        // Wrap into a FileSpec to reuse file_deploy.
        bp::FileSpec fs_spec;
        fs_spec.target_path = rendered->target_path.string();
        fs_spec.content = rendered->content;
        fs_spec.mode = bp::FileMode::DropIn;  // renderers always drop-in
        auto deployed = luban::file_deploy::deploy(fs_spec, next_id);
        if (!deployed) {
            return std::unexpected("deploy " + cfg.name + ": " + deployed.error());
        }
        log::infof("  rendered -> {}", deployed->target_path.string());

        gen::FileRecord rec;
        rec.from_blueprint = spec.name;
        rec.target_path = deployed->target_path.string();
        rec.content_sha256 = deployed->content_sha256;
        rec.mode = deployed->mode;
        if (deployed->backup_path) {
            rec.backup_path = deployed->backup_path->string();
        }
        next_gen.files[rec.target_path] = std::move(rec);
        ++result.files_deployed;
    }

    // ---- Files ---------------------------------------------------------
    const size_t total_files = spec.files.size();
    size_t file_idx = 0;
    for (auto& fspec : spec.files) {
        ++file_idx;
        log::stepf("[{}/{}] file: {}", file_idx, total_files, fspec.target_path);
        if (opts.dry_run) {
            ++result.files_deployed;
            continue;
        }
        auto deployed = luban::file_deploy::deploy(fspec, next_id, spec.name);
        if (!deployed) {
            return std::unexpected("deploy file " + fspec.target_path + ": " +
                                   deployed.error());
        }
        log::infof("  deployed -> {}", deployed->target_path.string());
        gen::FileRecord rec;
        rec.from_blueprint = spec.name;
        rec.target_path = deployed->target_path.string();
        rec.content_sha256 = deployed->content_sha256;
        rec.mode = deployed->mode;
        if (deployed->backup_path) {
            rec.backup_path = deployed->backup_path->string();
        }
        next_gen.files[rec.target_path] = std::move(rec);
        ++result.files_deployed;
    }

    // ---- Promote new generation ---------------------------------------
    if (opts.dry_run) return result;

    if (auto w = gen::write(next_gen); !w) {
        return std::unexpected("write generation: " + w.error());
    }
    if (auto s = gen::set_current(next_id); !s) {
        return std::unexpected("set current: " + s.error());
    }
    log::okf("applied generation {} ({} tool(s), {} config(s), {} file(s))",
             next_id, result.tools_fetched + result.tools_external,
             total_cfgs, total_files);
    return result;
}

}  // namespace luban::blueprint_apply
