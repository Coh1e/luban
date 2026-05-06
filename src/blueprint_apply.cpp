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
//    a [files."<path>"] entry with mode = drop-in. Down the road
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
#include <sstream>
#include <system_error>

#include "external_skip.hpp"
#include "file_deploy.hpp"
#include "generation.hpp"
#include "log.hpp"
#include "paths.hpp"
#include "platform.hpp"
#include "proc.hpp"
#include "program_renderer.hpp"
#include "source_resolver.hpp"
#include "store.hpp"
#include "xdg_shim.hpp"

namespace luban::blueprint_apply {

namespace {

namespace fs = std::filesystem;
namespace bp = luban::blueprint;
namespace bpl = luban::blueprint_lock;
namespace gen = luban::generation;

/// Resolve a post_install script path to an absolute path inside the
/// store_dir, with a traversal guard. Returns the absolute script path
/// if valid, or unexpected with an error message describing why not.
///
/// Two checks:
///   1. After lexically normalizing `<store_dir>/<rel>`, the result must
///      still start with `store_dir` (rejects "../../etc/passwd").
///   2. The resolved path must exist as a regular file (rejects typos
///      and missing scripts).
std::expected<fs::path, std::string> resolve_post_install(
    const fs::path& store_dir, std::string_view rel) {
    fs::path joined = store_dir / fs::path(std::string(rel));
    fs::path normalized = joined.lexically_normal();

    // Require the normalized path to be lexically inside store_dir.
    // lexically_relative returns something starting with ".." if it
    // escapes; guard that. Use string() rather than native() so the
    // comparison is portable across Windows wide vs POSIX narrow paths.
    fs::path probe = normalized.lexically_relative(store_dir);
    std::string probe_s = probe.string();
    if (probe_s.empty() || probe_s.rfind("..", 0) == 0) {
        return std::unexpected("post_install path escapes artifact root: " +
                               std::string(rel));
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
                                               *tool.post_install);
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
        std::string primary_shim;
        std::string primary_bin_rel;
        std::vector<std::string> secondary_shims;
        std::vector<std::string> secondary_bin_rels;
        if (tool.shims.empty()) {
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
            for (auto const& rel : tool.shims) {
                fs::path joined     = fetched->store_dir / fs::path(rel);
                fs::path normalized = joined.lexically_normal();
                fs::path probe      = normalized.lexically_relative(fetched->store_dir);
                std::string probe_s = probe.string();
                if (probe_s.empty() || probe_s.rfind("..", 0) == 0) {
                    return std::unexpected("shim " + tool.name +
                                           ": shims entry escapes artifact root: " + rel);
                }
                std::error_code ec;
                if (!fs::is_regular_file(normalized, ec)) {
                    return std::unexpected("shim " + tool.name +
                                           ": shims target not found: " + normalized.string());
                }
                std::string alias = normalized.stem().string();
                if (alias.empty()) {
                    return std::unexpected("shim " + tool.name +
                                           ": cannot derive alias from `" + rel + "`");
                }
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
            }
            log::infof("  shims: {} alias(es) -> {}",
                       tool.shims.size(), paths::xdg_bin_home().string());
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

    // ---- Programs ------------------------------------------------------
    luban::program_renderer::Context ctx;
    ctx.home = paths::home();
    ctx.xdg_config = paths::config_dir();
    ctx.blueprint_name = spec.name;
    ctx.platform = std::string(luban::platform::host_os());

    const size_t total_progs = spec.programs.size();
    size_t prog_idx = 0;
    for (auto& prog : spec.programs) {
        ++prog_idx;
        log::stepf("[{}/{}] config: {}", prog_idx, total_progs, prog.name);
        auto rendered = luban::program_renderer::render(prog.name, prog.config, ctx);
        if (!rendered) {
            return std::unexpected("render " + prog.name + ": " + rendered.error());
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
            return std::unexpected("deploy " + prog.name + ": " + deployed.error());
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
        auto deployed = luban::file_deploy::deploy(fspec, next_id);
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
             total_progs, total_files);
    return result;
}

}  // namespace luban::blueprint_apply
