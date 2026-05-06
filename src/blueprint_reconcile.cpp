// See `blueprint_reconcile.hpp`.
//
// Best-effort: per-step file/shim ops that fail get logged to
// ReconcileResult.warnings and the walk keeps going. The alternative
// (abort on first error) would leave half-reconciled state with no
// recovery path; warnings + completion let the user inspect what
// remained inconsistent and decide whether to re-run `bp apply` to
// renormalize.

#include "blueprint_reconcile.hpp"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <system_error>

#include "file_deploy.hpp"
#include "generation.hpp"
#include "store.hpp"
#include "xdg_shim.hpp"

namespace luban::blueprint_reconcile {

namespace {

namespace fs = std::filesystem;
namespace gen = luban::generation;
namespace fd = luban::file_deploy;

/// Promote a `gen::FileRecord` back into a `file_deploy::DeployedFile`
/// so we can call `fd::restore` against it. Same shape, different home —
/// generation tracks records as JSON-friendly POD, file_deploy as the
/// in-memory return of deploy().
fd::DeployedFile to_deployed(const gen::FileRecord& f) {
    fd::DeployedFile d;
    d.target_path = fd::expand_home(f.target_path);
    d.content_sha256 = f.content_sha256;
    d.mode = f.mode;
    if (f.backup_path) d.backup_path = *f.backup_path;
    return d;
}

/// Rebuild every shim a tool installed: primary + all secondary entries.
/// Uses `record.artifact_id` to find the artifact in the store and the
/// `bin_path_rel` fields to locate each binary inside it. Shim aliases
/// are derived from the previously-recorded shim filenames so a
/// recreated shim ends up at the same path the original did.
void recreate_shims_for(const std::string& tool_name,
                        const gen::ToolRecord& rec,
                        ReconcileResult& res) {
    if (rec.is_external) return;       // luban never shimmed it
    if (rec.artifact_id.empty()) return;

    if (rec.bin_path_rel.empty() || rec.shim_path.empty()) {
        res.warnings.push_back("cannot re-shim `" + tool_name +
                               "`: legacy record missing bin_path_rel "
                               "(was applied with a pre-rollback luban; "
                               "re-run `luban bp apply` to refresh)");
        return;
    }

    auto store_dir = luban::store::store_path(rec.artifact_id);
    std::error_code ec;
    if (!fs::is_directory(store_dir, ec)) {
        res.warnings.push_back("cannot re-shim `" + tool_name +
                               "`: artifact " + rec.artifact_id +
                               " not present in store (gc'd? clean reinstall?)");
        return;
    }

    // Primary
    {
        auto exe = (store_dir / fs::path(rec.bin_path_rel)).lexically_normal();
        std::string alias = fs::path(rec.shim_path).stem().string();
        if (alias.empty()) {
            res.warnings.push_back("cannot re-shim `" + tool_name +
                                   "`: cannot derive alias from " +
                                   rec.shim_path);
        } else if (auto s = luban::xdg_shim::write_cmd_shim(alias, exe); !s) {
            res.warnings.push_back("re-shim `" + tool_name + "`: " + s.error());
        } else {
            ++res.shims_recreated;
        }
    }

    // Secondary shims (multi-binary tools)
    for (size_t i = 0; i < rec.shim_paths_secondary.size(); ++i) {
        const auto& sp = rec.shim_paths_secondary[i];
        const std::string& bp_rel = (i < rec.bin_paths_rel_secondary.size())
            ? rec.bin_paths_rel_secondary[i] : std::string();
        if (bp_rel.empty() || sp.empty()) {
            res.warnings.push_back("cannot re-shim secondary alias for `" +
                                   tool_name + "`: missing path data");
            continue;
        }
        auto exe = (store_dir / fs::path(bp_rel)).lexically_normal();
        std::string alias = fs::path(sp).stem().string();
        if (alias.empty()) {
            res.warnings.push_back("cannot re-shim secondary for `" +
                                   tool_name + "`: cannot derive alias from " + sp);
            continue;
        }
        if (auto s = luban::xdg_shim::write_cmd_shim(alias, exe); !s) {
            res.warnings.push_back("re-shim secondary `" + tool_name +
                                   "` (" + alias + "): " + s.error());
        } else {
            ++res.shims_recreated;
        }
    }
}

/// Remove every shim a tool installed (primary + all secondary).
void remove_shims_for(const gen::ToolRecord& rec, ReconcileResult& res) {
    if (rec.is_external) return;
    if (!rec.shim_path.empty()) {
        if (auto r = luban::xdg_shim::remove_cmd_shim(rec.shim_path); !r) {
            res.warnings.push_back("remove shim " + rec.shim_path + ": " + r.error());
        } else {
            ++res.shims_removed;
        }
    }
    for (auto& sp : rec.shim_paths_secondary) {
        if (sp.empty()) continue;
        if (auto r = luban::xdg_shim::remove_cmd_shim(sp); !r) {
            res.warnings.push_back("remove shim " + sp + ": " + r.error());
        } else {
            ++res.shims_removed;
        }
    }
}

/// Undo `higher` to bring on-disk state back to `lower`.
void reconcile_step(const gen::Generation& higher,
                    const gen::Generation& lower,
                    ReconcileResult& res) {
    // Files modified or added at `higher` → restore from backup chain.
    for (auto& [path, fh] : higher.files) {
        auto it = lower.files.find(path);
        bool unchanged = (it != lower.files.end() &&
                          it->second.content_sha256 == fh.content_sha256);
        if (unchanged) continue;
        auto deployed = to_deployed(fh);
        if (auto r = fd::restore(deployed); !r) {
            res.warnings.push_back("restore " + path + ": " + r.error());
        } else {
            ++res.files_restored;
        }
    }

    // Files dropped at `higher` → recreate from content store using
    // `lower`'s recorded sha. (The backup chain doesn't help here;
    // backup_path stores the content from before lower's deploy, not
    // lower's own content.)
    for (auto& [path, fl] : lower.files) {
        if (higher.files.count(path)) continue;
        if (fl.content_sha256.empty()) {
            res.warnings.push_back("cannot recreate " + path +
                                   ": legacy record missing content_sha256");
            continue;
        }
        auto target = fd::expand_home(fl.target_path);
        if (auto r = fd::recreate_from_content_store(fl.content_sha256, target); !r) {
            res.warnings.push_back("recreate " + path + ": " + r.error());
        } else {
            ++res.files_recreated;
        }
    }

    // Tools added or changed at `higher` → remove higher's shims.
    for (auto& [name, th] : higher.tools) {
        auto it = lower.tools.find(name);
        bool unchanged = (it != lower.tools.end() &&
                          it->second.artifact_id == th.artifact_id &&
                          it->second.is_external == th.is_external);
        if (unchanged) continue;
        remove_shims_for(th, res);
    }

    // Tools dropped at `higher` OR changed at `higher` → rebuild from
    // `lower`'s artifact. The "changed" case removed higher's shims
    // above; we now write the lower-version shims fresh on top of
    // (now-empty) ~/.local/bin paths.
    for (auto& [name, tl] : lower.tools) {
        auto it = higher.tools.find(name);
        bool unchanged = (it != higher.tools.end() &&
                          it->second.artifact_id == tl.artifact_id &&
                          it->second.is_external == tl.is_external);
        if (unchanged) continue;
        recreate_shims_for(name, tl, res);
    }
}

}  // namespace

std::expected<ReconcileResult, std::string> reconcile_to(int target_id) {
    ReconcileResult res;
    res.target_id = target_id;

    auto cur_opt = gen::get_current();
    if (!cur_opt) {
        // No current generation — nothing on disk we'd be undoing.
        // Treat as a no-op success; caller (`bp rollback`) checks for
        // current-vs-target separately.
        return res;
    }
    int cur = *cur_opt;
    if (cur == target_id) return res;
    if (target_id > cur) {
        return std::unexpected(
            "forward reconcile (target > current) not supported; "
            "this path is reserved for `bp rollback` only");
    }

    auto target = gen::read(target_id);
    if (!target) return std::unexpected("read target gen: " + target.error());

    // Build the descending chain of generation ids in (target, cur].
    // Non-consecutive ids are tolerated (gc not implemented yet, but the
    // walk doesn't assume contiguity).
    auto all_ids = gen::list_ids();
    std::vector<int> chain;
    for (int id : all_ids) {
        if (id > target_id && id <= cur) chain.push_back(id);
    }
    std::sort(chain.begin(), chain.end(), std::greater<int>());

    // For each pair (higher = chain[k], lower = chain[k+1] OR target),
    // reconcile one step.
    for (size_t k = 0; k < chain.size(); ++k) {
        int higher_id = chain[k];
        int lower_id = (k + 1 < chain.size()) ? chain[k + 1] : target_id;

        auto higher = gen::read(higher_id);
        if (!higher) {
            res.warnings.push_back("read gen " + std::to_string(higher_id) +
                                   ": " + higher.error());
            continue;
        }
        auto lower = gen::read(lower_id);
        if (!lower) {
            res.warnings.push_back("read gen " + std::to_string(lower_id) +
                                   ": " + lower.error());
            continue;
        }
        reconcile_step(*higher, *lower, res);
    }

    return res;
}

}  // namespace luban::blueprint_reconcile
