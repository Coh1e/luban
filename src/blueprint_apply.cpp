// See `blueprint_apply.hpp`.
//
// The orchestrator is mostly mechanical wiring: call the right S3-S5
// module in the right order and feed each step's output into the next.
//
// v0.5.0+ simplifications (DESIGN §11 drops generation history):
//   - No in-memory Generation built; no <state>/generations/N.json
//     written. The two pieces of state we genuinely need across runs —
//     "which bps did the user apply" + "which shims did luban create" —
//     live in flat-file form under <state>/luban/ via applied_db.
//   - meta.requires gating consults applied_db::is_applied instead of
//     reading the previous generation's applied_blueprints.
//   - Re-applies are NOT a clean "drop previous contributions then
//     replay"; file_deploy already detects content collisions and
//     keeps a backup, and shim writers overwrite idempotently. Net
//     result: re-apply lands the same final state.
//
// Shim writing at ~/.local/bin/ is delegated to src/xdg_shim.cpp;
// the .cmd format matches src/shim.cpp's text shim.

#include "blueprint_apply.hpp"

#include <fstream>
#include <set>
#include <sstream>
#include <system_error>
#include <vector>

#include "applied_db.hpp"
#include "external_skip.hpp"
#include "file_deploy.hpp"
#include "log.hpp"
#include "paths.hpp"
#include "platform.hpp"
#include "proc.hpp"
#include "config_renderer.hpp"
#include "renderer_registry.hpp"
#include "source_resolver.hpp"
#include "store.hpp"
#include "xdg_shim.hpp"

namespace luban::blueprint_apply {

namespace {

namespace fs = std::filesystem;
namespace bp = luban::blueprint;
namespace bpl = luban::blueprint_lock;

/// Resolve a post_install script path to an absolute path with a
/// traversal guard.
///
/// Two roots possible:
///   - default (no prefix): path is artifact-relative — script lives
///     inside the extracted upstream archive (vcpkg's bootstrap-vcpkg.bat
///     is the canonical case). Guard against escaping store_dir.
///   - `bp:` prefix:        path is bp-source-relative — script lives in
///     the bp source repo alongside the .lua blueprint file (Maple Mono
///     font registration is the canonical case — upstream ships only
///     .ttf files, the registration logic has to come from the bp).
///     Guard against escaping bp_source_root. Errors loudly if
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
/// match host_triplet() exactly — no fallback chain.
const bpl::LockedPlatform* pick_platform(const bpl::LockedTool& t) {
    auto triplet = luban::platform::host_triplet();
    auto it = t.platforms.find(triplet);
    if (it == t.platforms.end()) return nullptr;
    return &it->second;
}

}  // namespace

std::expected<ApplyResult, std::string> apply(const bp::BlueprintSpec& spec,
                                              const bpl::BlueprintLock& lock,
                                              const ApplyOptions& opts) {
    ApplyResult result;

    // ---- Preflight: meta.requires gating (DESIGN §4) ---------------------
    //
    // Each name in spec.meta.requires_ must appear in <state>/luban/
    // applied.txt. Hard error otherwise — listing every missing dep + the
    // exact `luban bp apply` line to fix it. We do NOT auto-recurse into
    // deps; explicit beats clever for the layered foundation/cpp-toolchain
    // story.
    //
    // Qualifier handling: bp authors write `requires = ["main/bootstrap"]`
    // (qualified) but applied_db stores bare names. is_applied strips
    // `<source>/` qualifier on both sides, so the comparison is bare-vs-bare.
    if (!spec.meta.requires_.empty()) {
        std::vector<std::string> missing;
        for (auto& req : spec.meta.requires_) {
            if (!luban::applied_db::is_applied(req)) missing.push_back(req);
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

    // Collect the absolute shim paths created during this apply so we can
    // record them into owned-shims.txt at the end. Recorded only on
    // success (errors short-circuit before the write).
    std::vector<fs::path> created_shims;

    // ---- Tools ---------------------------------------------------------
    const size_t total_tools = spec.tools.size();
    size_t tool_idx = 0;
    for (auto& tool : spec.tools) {
        ++tool_idx;
        log::stepf("[{}/{}] tool: {}", tool_idx, total_tools, tool.name);

        // Step 1: external skip. Probe target defaults to `tool.name`;
        // overridden by `external_skip` when the brand and the canonical
        // PATH binary differ (openssh ↔ ssh.exe is the load-bearing case).
        std::string probe_target = tool.external_skip.value_or(tool.name);
        if (auto ext = luban::external_skip::probe(probe_target)) {
            ++result.tools_external;
            log::infof("  external (provided by {})", ext->resolved_path.string());
            continue;
        }

        // Step 2: lookup in lock.
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

        // Step 3.5: post_install hook. Skip on cache hits — post_install
        // is part of "install" and content-addressed artifacts are
        // immutable. Fresh extraction always runs it.
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

        // no_shim = true bypasses Step 4 entirely — used for "tools" that
        // register themselves through non-PATH channels (fonts via
        // HKCU\...\Fonts + AddFontResourceEx, etc).
        if (tool.no_shim) {
            ++result.tools_fetched;
            log::infof("  no_shim — registration handled by post_install");
            continue;
        }

        // Step 4: shim(s) under ~/.local/bin/.
        //
        // Single-shim default: `plat->bin` basename as alias.
        // Multi-shim: explicit `tool.shims` list + optional `tool.shim_dir`
        // (auto-discover .exe under <store>/<dir>/). Each shim path is
        // appended to created_shims so we can later record them in
        // owned-shims.txt. Aliases dedup within this tool.
        std::set<std::string> aliases_seen;

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
            created_shims.push_back(*shim);
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
            created_shims.push_back(*shim);
        } else {
            for (auto const& rel : tool.shims) {
                if (auto r = add_shim(fetched->store_dir / fs::path(rel), rel); !r) {
                    return std::unexpected(r.error());
                }
            }
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
        const std::string& renderer_name = cfg.for_tool.value_or(cfg.name);
        log::stepf("[{}/{}] config: {}", cfg_idx, total_cfgs, cfg.name);
        if (!opts.renderer_registry) {
            return std::unexpected("render " + cfg.name +
                                   ": no renderer registry — caller must"
                                   " supply one");
        }
        auto rendered = luban::config_renderer::render_with_registry(
            *opts.renderer_registry, renderer_name, cfg.config, ctx);
        if (!rendered) {
            return std::unexpected("render " + cfg.name + ": " + rendered.error());
        }
        if (opts.dry_run) {
            log::infof("  (dry-run) would render -> {}",
                       rendered->target_path.string());
            ++result.files_deployed;
            continue;
        }

        bp::FileSpec fs_spec;
        fs_spec.target_path = rendered->target_path.string();
        fs_spec.content = rendered->content;
        fs_spec.mode = bp::FileMode::DropIn;
        // generation_id = 0: backup buckets share a single namespace
        // post-MVP; we don't roll back across applies anymore.
        auto deployed = luban::file_deploy::deploy(fs_spec, 0);
        if (!deployed) {
            return std::unexpected("deploy " + cfg.name + ": " + deployed.error());
        }
        log::infof("  rendered -> {}", deployed->target_path.string());
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
        auto deployed = luban::file_deploy::deploy(fspec, 0, spec.name);
        if (!deployed) {
            return std::unexpected("deploy file " + fspec.target_path + ": " +
                                   deployed.error());
        }
        log::infof("  deployed -> {}", deployed->target_path.string());
        ++result.files_deployed;
    }

    // ---- Promote success: applied.txt + owned-shims.txt --------------
    if (opts.dry_run) return result;

    if (!luban::applied_db::mark_applied(spec.name)) {
        log::warnf("could not write {}; subsequent `meta.requires` checks "
                   "may falsely report this bp as missing",
                   luban::applied_db::applied_path().string());
    }
    for (auto& shim : created_shims) {
        if (!luban::applied_db::record_owned_shim(shim)) {
            log::warnf("could not record shim {} into owned-shims.txt; "
                       "self uninstall may leave it behind",
                       shim.string());
        }
    }

    log::okf("applied `{}` ({} tool(s), {} config(s), {} file(s))",
             spec.name, result.tools_fetched + result.tools_external,
             total_cfgs, total_files);
    return result;
}

}  // namespace luban::blueprint_apply
