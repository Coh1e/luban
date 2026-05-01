#pragma once
// 1:1 port of luban_boot/component.py 的 install/uninstall pipeline。
//
// install pipeline:
//   1. manifest_source::load(name) — overlay or in-tree seed only, no network
//   2. parse via scoop_manifest（含安全白名单）
//   3. 算 toolchain_dir 名 = "<name>-<version>-<arch>"
//   4. 幂等：installed.json 已记录同版本且目录存在 → 直接返回
//   5. download archive 到 cache（带 SHA 校验，缓存命中也再校验一次）
//   6. extract 到 staging dir（toolchains/.tmp-...）
//   7. apply extract_dir：定位 toolchain 真根
//   8. promote staging → final（os::rename，跨卷 fallback copy）
//   9. 给每个 bin 写 shim
//  10. 写 installed.json record（atomic）
//
// 不做：per-file CAS hardlink（plan §0.5.5 留 M3）。

#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace luban::component {

namespace fs = std::filesystem;

struct InstallReport {
    std::string name;
    std::string version;
    fs::path toolchain_dir;
    std::vector<std::pair<std::string, fs::path>> bins;  // (alias, exe)
    bool was_already_installed = false;                   // 幂等命中
};

enum class ErrorKind {
    ManifestNotFound,
    UnsafeManifest,
    IncompleteManifest,
    DownloadFailed,
    HashMismatch,
    ExtractFailed,
    Filesystem,
};

struct Error {
    ErrorKind kind;
    std::string message;
};

std::expected<InstallReport, Error> install(const std::string& name,
                                            bool force = false,
                                            const std::string& arch = "x86_64");

bool uninstall(const std::string& name);

}  // namespace luban::component
