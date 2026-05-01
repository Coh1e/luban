#include "component.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <format>
#include <fstream>
#include <sstream>
#include <system_error>

#include "archive.hpp"
#include "manifest_source.hpp"
#include "download.hpp"
#include "hash.hpp"
#include "log.hpp"
#include "paths.hpp"
#include "proc.hpp"
#include "registry.hpp"
#include "scoop_manifest.hpp"
#include "shim.hpp"

namespace luban::component {

namespace {

std::string toolchain_dir_name(const scoop_manifest::ResolvedManifest& m) {
    std::string safe_ver = m.version;
    std::replace(safe_ver.begin(), safe_ver.end(), '/', '-');
    std::replace(safe_ver.begin(), safe_ver.end(), ':', '-');
    return std::format("{}-{}-{}", m.name, safe_ver, m.architecture);
}

fs::path final_path(const std::string& tc_dir_name) {
    return paths::toolchains_dir() / tc_dir_name;
}

fs::path staging_path(const std::string& tc_dir_name) {
    return paths::toolchains_dir() / (".tmp-" + tc_dir_name);
}

// 把 bin rel 路径里的分隔符归一化（manifest 里大多是 '/'）
fs::path resolve_bin(const fs::path& root, const std::string& rel) {
    std::string norm = rel;
    for (auto& c : norm) {
        if (c == '/' || c == '\\') c = static_cast<char>(fs::path::preferred_separator);
    }
    return root / norm;
}

// staging 内按 extract_dir 选出 toolchain 真根（manifest 指明 wrapper 名）
fs::path resolve_toolchain_root(const fs::path& staging,
                                const scoop_manifest::ResolvedManifest& m) {
    if (!m.extract_dir) return staging;
    std::error_code ec;
    fs::path candidate = staging / *m.extract_dir;
    if (fs::is_directory(candidate, ec)) return candidate;
    // 尝试斜杠归一化
    std::string norm = *m.extract_dir;
    for (auto& c : norm) {
        if (c == '/' || c == '\\') c = static_cast<char>(fs::path::preferred_separator);
    }
    candidate = staging / norm;
    if (fs::is_directory(candidate, ec)) return candidate;
    return staging;
}

void wipe(const fs::path& p) {
    std::error_code ec;
    if (fs::is_symlink(p, ec) || fs::is_regular_file(p, ec)) {
        fs::remove(p, ec);
    } else if (fs::exists(p, ec)) {
        fs::remove_all(p, ec);
    }
}

std::string archive_filename_from_url(const std::string& url, const std::string& fallback) {
    auto pos = url.rfind('/');
    if (pos == std::string::npos || pos + 1 >= url.size()) return fallback;
    std::string name = url.substr(pos + 1);
    auto q = name.find('?');
    if (q != std::string::npos) name = name.substr(0, q);
    return name.empty() ? fallback : name;
}

std::string now_iso() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

// Pre-v0.2 had a source_label(path) helper that parsed the bucket name out
// of the manifest's on-disk location. With bucket_sync gone, manifest_source
// returns the label directly via LoadResult::source_label ("overlay" or
// "seed"); nothing computes it from the path anymore.
[[maybe_unused]] std::string source_label_unused() {
    return "";
}

}  // namespace

std::expected<InstallReport, Error> install(const std::string& name, bool force,
                                            const std::string& arch) {
    paths::ensure_dirs();

    // 1. Locate manifest. Only overlay + in-tree seed are searched (no
    //    network); see manifest_source.hpp for the reasoning.
    auto fetched = manifest_source::load(name);
    if (!fetched) {
        return std::unexpected(Error{ErrorKind::ManifestNotFound,
            "no manifest for '" + name + "' in overlay or manifests_seed/. "
            "Add a manifests_seed/" + name + ".json (commit it) to install."});
    }

    // 2. Parse + safety whitelist (rejects installer.script / pre_install /
    //    .msi / etc. — see scoop_manifest.cpp).
    scoop_manifest::ResolvedManifest parsed;
    try {
        parsed = scoop_manifest::parse(fetched->manifest, name, arch);
    } catch (scoop_manifest::UnsafeManifest& e) {
        return std::unexpected(Error{ErrorKind::UnsafeManifest, e.what()});
    } catch (scoop_manifest::IncompleteManifest& e) {
        return std::unexpected(Error{ErrorKind::IncompleteManifest, e.what()});
    }

    // 3. plan 路径
    std::string tc_name = toolchain_dir_name(parsed);
    fs::path final = final_path(tc_name);

    // 4. 幂等检查
    auto installed = registry::load_installed();
    auto it = installed.find(name);
    std::error_code ec;
    if (!force && it != installed.end()
        && it->second.version == parsed.version
        && fs::exists(final, ec)) {
        log::okf("{} {} already installed at {}", name, parsed.version, final.string());
        InstallReport r;
        r.name = name;
        r.version = parsed.version;
        r.toolchain_dir = final;
        for (auto& [alias, rel] : it->second.bins) {
            r.bins.emplace_back(alias, resolve_bin(final, rel));
        }
        r.was_already_installed = true;
        return r;
    }

    // 5. download archive（带 SHA 校验）
    std::string archive_name = archive_filename_from_url(
        parsed.url, parsed.name + "-" + parsed.version + ".bin");
    fs::path archive_path = paths::downloads_dir() / archive_name;

    auto expected_hash = hash::parse(parsed.hash_spec);
    if (!expected_hash) {
        return std::unexpected(Error{ErrorKind::IncompleteManifest,
            "bad hash spec: " + parsed.hash_spec});
    }

    // 缓存命中也再校验一次
    bool cached_ok = false;
    if (fs::exists(archive_path, ec)) {
        if (hash::verify_file(archive_path, *expected_hash)) {
            cached_ok = true;
        } else {
            log::warnf("cached {} hash mismatch; re-downloading", archive_name);
            fs::remove(archive_path, ec);
        }
    }
    if (!cached_ok) {
        fs::create_directories(archive_path.parent_path(), ec);
        download::DownloadOptions opts;
        opts.expected_hash = *expected_hash;
        opts.label = archive_name;
        opts.retries = 3;
        opts.timeout_seconds = 60;

        // Try the canonical URL, then each mirror in order. A hash mismatch
        // is FATAL (don't iterate to mirrors — that means the published hash
        // is wrong, and trying mirrors won't help). Network errors fall
        // through to the next URL. Same hash applies to whichever URL serves.
        std::vector<std::string> candidates = {parsed.url};
        for (auto& m : parsed.mirrors) candidates.push_back(m);

        std::optional<download::Error> last_error;
        bool ok = false;
        for (size_t i = 0; i < candidates.size(); ++i) {
            log::stepf("download {} ({} of {}): {}",
                       archive_name, i + 1, candidates.size(), candidates[i]);
            auto dl = download::download(candidates[i], archive_path, opts);
            if (dl) { ok = true; break; }
            last_error = dl.error();
            if (dl.error().kind == download::ErrorKind::HashMismatch) {
                // Bytes mismatched the expected hash — don't try mirrors,
                // that's a manifest bug. Return immediately.
                return std::unexpected(Error{ErrorKind::HashMismatch, dl.error().message});
            }
            if (i + 1 < candidates.size()) {
                log::warnf("  failed: {}; trying next mirror", dl.error().message);
            }
        }
        if (!ok) {
            return std::unexpected(Error{ErrorKind::DownloadFailed,
                last_error ? last_error->message
                           : std::string("all mirrors failed")});
        }
    }

    // 6. extract 到 staging
    fs::path staging = staging_path(tc_name);
    if (fs::exists(staging, ec)) wipe(staging);
    fs::create_directories(staging, ec);
    log::stepf("extract {}", archive_name);
    if (auto rc = archive::extract(archive_path, staging); !rc) {
        wipe(staging);
        return std::unexpected(Error{ErrorKind::ExtractFailed, rc.error().message});
    }

    // 7. extract_dir：选 toolchain 真根
    fs::path tc_root = resolve_toolchain_root(staging, parsed);

    // 8. promote staging → final
    if (fs::exists(final, ec)) wipe(final);
    fs::create_directories(final.parent_path(), ec);
    fs::rename(tc_root, final, ec);
    if (ec) {
        // 跨卷 fallback：递归 copy
        ec.clear();
        fs::copy(tc_root, final, fs::copy_options::recursive, ec);
        if (ec) {
            wipe(staging);
            return std::unexpected(Error{ErrorKind::Filesystem,
                "rename/copy failed: " + ec.message()});
        }
    }
    if (tc_root != staging) wipe(staging);  // 清理空 wrapper

    // 8b. vcpkg 特殊路径：extract 出来的源树没有 vcpkg.exe；跑一次 bootstrap-vcpkg.bat
    //     就地下载匹配的 vcpkg.exe 到 final 根目录。这是 vcpkg 上游 blessed 的安装步骤，
    //     不是 manifest 注入的脚本——所以不违反"不执行 manifest 脚本"的安全策略。
    //     bootstrap-vcpkg.bat 内部读 scripts/vcpkg-tool-metadata.txt 选 vcpkg-tool 版本，
    //     从 microsoft/vcpkg-tool/releases 下载。需要 PATH 上有 mingit（cert + curl）。
    if (name == "vcpkg") {
        fs::path bootstrap = final / "bootstrap-vcpkg.bat";
        fs::path vcpkg_exe = final / "vcpkg.exe";
        if (!fs::exists(vcpkg_exe, ec) && fs::exists(bootstrap, ec)) {
            log::stepf("bootstrapping vcpkg.exe (microsoft/vcpkg-tool)");
            std::map<std::string, std::string> env_overrides;
            // 让 bootstrap 自己用系统 cmd.exe，不需特别注入。
            int rc = proc::run({"cmd.exe", "/c", bootstrap.string(), "-disableMetrics"},
                               final.string(), env_overrides);
            if (rc != 0) {
                log::warnf("bootstrap-vcpkg.bat exited {}; vcpkg may be incomplete", rc);
            } else {
                log::ok("vcpkg.exe ready");
            }
        }
    }

    // 8c. emscripten special-case: write the python-style config so emcc /
    //     emcmake can locate LLVM_ROOT / BINARYEN_ROOT / EMSCRIPTEN_ROOT /
    //     NODE_JS without an emsdk activation step.
    //
    //     Path: <config>/emscripten/config (XDG-respecting). Older luban
    //     (pre-v0.2) wrote it next to the install at <install>/emscripten/
    //     .emscripten — but that location has two problems:
    //       (1) it's data-dir-side state under config-dir's job, breaking
    //           our 4-home contract;
    //       (2) `emcc` looks at $EM_CONFIG when set, otherwise probes a hard-
    //           coded list (~/.emscripten on Linux, %USERPROFILE%\.emscripten
    //           on Windows). We expose EM_CONFIG via env_snapshot + HKCU so
    //           the file's actual location is decoupled from where luban
    //           drops the toolchain.
    //
    //     Looks up node from registry; if missing, falls back to PATH-resolved
    //     node so emcc can at least try.
    if (name == "emscripten") {
        fs::path config = paths::config_dir() / "emscripten" / "config";
        std::string node_path;
        {
            auto recs = registry::load_installed();
            auto it = recs.find("node");
            if (it != recs.end()) {
                fs::path node_root = paths::toolchain_dir(it->second.toolchain_dir);
                fs::path node_exe = node_root / "node.exe";
                if (fs::exists(node_exe, ec)) {
                    node_path = node_exe.string();
                    std::replace(node_path.begin(), node_path.end(), '\\', '/');
                }
            }
        }
        std::string llvm_root = (final / "bin").string();
        std::string binaryen_root = final.string();
        std::string emscripten_root = (final / "emscripten").string();
        for (auto* p : {&llvm_root, &binaryen_root, &emscripten_root}) {
            std::replace(p->begin(), p->end(), '\\', '/');
        }
        std::ostringstream cfg;
        cfg << "# generated by luban — DO NOT EDIT BY HAND\n";
        cfg << "# luban regenerates this every `luban setup --with emscripten`.\n";
        cfg << "# emcc reads via $EM_CONFIG (set by `luban env --user` or env_snapshot).\n";
        cfg << "LLVM_ROOT = '" << llvm_root << "'\n";
        cfg << "BINARYEN_ROOT = '" << binaryen_root << "'\n";
        cfg << "EMSCRIPTEN_ROOT = '" << emscripten_root << "'\n";
        if (!node_path.empty()) {
            cfg << "NODE_JS = '" << node_path << "'\n";
        } else {
            log::warn("node not in registry; emscripten will fall back to PATH-resolved node");
            cfg << "NODE_JS = 'node'\n";
        }
        cfg << "COMPILER_ENGINE = NODE_JS\n";
        cfg << "JS_ENGINES = [NODE_JS]\n";
        std::error_code ec2;
        fs::create_directories(config.parent_path(), ec2);
        std::ofstream out(config, std::ios::binary | std::ios::trunc);
        std::string text = cfg.str();
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        log::okf("emscripten config written \xe2\x86\x92 {}", config.string());
        log::info("set EM_CONFIG=<that path> in your shell, or run `luban env --user` to register it on HKCU");
    }

    // 9. 写 shim + 收集 bin 路径
    //
    // bin_dir() is <data>/bin/ (cargo's ~/.cargo/bin/ pattern — luban-owned,
    // not shared with other XDG bin tenants). Collisions there should be
    // rare since users don't manually drop files into luban's data dir, but
    // shim::write_shim still checks the .shim-table.json before clobbering.
    std::vector<std::pair<std::string, fs::path>> bins_out;
    std::vector<std::string> aliases_taken;
    for (auto& entry : parsed.bins) {
        fs::path exe = resolve_bin(final, entry.relative_path);
        if (!fs::exists(exe, ec)) {
            log::warnf("  bin '{}' points at missing file {}", entry.alias, exe.string());
            continue;
        }
        auto wr = shim::write_shim(entry.alias, exe, entry.prefix_args);
        if (wr == shim::WriteResult::Skipped) {
            log::warnf("  shim skipped: {} already exists in {} (not luban-managed)",
                       entry.alias, paths::bin_dir().string());
            log::info("  → run `luban shim --force` to overwrite");
        }
        bins_out.emplace_back(entry.alias, exe);
        aliases_taken.push_back(entry.alias);
    }
    // env_add_path 暴露的目录里的 *.exe / .cmd / .bat 也自动 shim（git 风格 manifest 用这个）
    for (auto& rel_dir : parsed.env_add_path) {
        std::string norm = rel_dir;
        for (auto& c : norm) {
            if (c == '/' || c == '\\') c = static_cast<char>(fs::path::preferred_separator);
        }
        fs::path dir_path = final / norm;
        if (!fs::is_directory(dir_path, ec)) continue;
        std::vector<fs::directory_entry> entries;
        for (auto& e : fs::directory_iterator(dir_path, ec)) entries.push_back(e);
        std::sort(entries.begin(), entries.end(),
                  [](auto& a, auto& b) { return a.path() < b.path(); });
        for (auto& e : entries) {
            if (!e.is_regular_file()) continue;
            std::string ext = e.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext != ".exe" && ext != ".cmd" && ext != ".bat") continue;
            std::string alias = e.path().stem().string();
            if (std::find(aliases_taken.begin(), aliases_taken.end(), alias) != aliases_taken.end()) continue;
            auto wr = shim::write_shim(alias, e.path());
            if (wr == shim::WriteResult::Skipped) {
                log::warnf("  shim skipped: {} already exists in {} (not luban-managed)",
                           alias, paths::bin_dir().string());
                continue;
            }
            bins_out.emplace_back(alias, e.path());
            aliases_taken.push_back(alias);
        }
    }

    // 10. 写 installed.json
    registry::ComponentRecord rec;
    rec.name = name;
    rec.version = parsed.version;
    rec.source = fetched->source_label;
    rec.url = parsed.url;
    rec.hash_spec = parsed.hash_spec;
    rec.toolchain_dir = tc_name;
    rec.architecture = parsed.architecture;
    rec.installed_at = now_iso();
    for (auto& [alias, exe] : bins_out) {
        std::string rel = fs::relative(exe, final, ec).string();
        std::replace(rel.begin(), rel.end(), '\\', '/');
        rec.bins.emplace_back(alias, rel);
    }

    auto recs = registry::load_installed();
    recs[name] = rec;
    registry::save_installed(recs);

    log::okf("installed {} {} → {}", name, parsed.version, final.string());

    InstallReport r;
    r.name = name;
    r.version = parsed.version;
    r.toolchain_dir = final;
    r.bins = std::move(bins_out);
    return r;
}

bool uninstall(const std::string& name) {
    auto recs = registry::load_installed();
    auto it = recs.find(name);
    if (it == recs.end()) return false;
    registry::ComponentRecord rec = it->second;
    recs.erase(it);
    registry::save_installed(recs);

    if (!rec.toolchain_dir.empty()) {
        fs::path target = paths::toolchains_dir() / rec.toolchain_dir;
        std::error_code ec;
        if (fs::exists(target, ec)) wipe(target);
    }
    for (auto& [alias, _] : rec.bins) {
        shim::remove_shim(alias);
    }
    return true;
}

}  // namespace luban::component
