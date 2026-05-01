# 技术栈

## 语言与编译

- **C++23**（`target_compile_features cxx_std_23`）
- **LLVM-MinGW**（gcc/clang ABI）；MSVC 构建尚未验证
- 静态链接 `-static -static-libgcc -static-libstdc++`，避免运行时依赖 DLL
- Release 二进制 ~3 MB，Debug ~31 MB

## 第三方依赖（全部 single-header vendor 在 `third_party/`）

| 库 | 用途 | License |
|---|---|---|
| `nlohmann/json` (`json.hpp`) | manifest / installed.json 读写 | MIT |
| `miniz` (`miniz.{h,c}`) | ZIP 解压 | BSD-3-Clause |
| `toml++` (`toml.hpp`) | `luban.toml` 解析 | MIT |

不引入任何动态库依赖。**新增依赖前必须先评估能否拒绝**，必要时也只接受 single-header。

## 平台 API

- **WinHTTP**：HTTPS 下载（`download.cpp`）
- **WinCrypt / BCrypt**：SHA256（`hash.cpp`）
- **SHGetKnownFolderPath**：`%LOCALAPPDATA%` 等解析（`paths.cpp`）
- **CreateProcessW**：子进程（`proc.cpp`、`shim_exe/main.cpp`）
- **Reg\* + WM_SETTINGCHANGE**：HKCU PATH 操作（`win_path.cpp`）

## 构建工具

- **CMake ≥ 3.25** + **Ninja**（用 luban 自身或宿主 cmake/ninja 都行）
- 两个 preset：`default`（Debug）和 `release`
- 没有独立测试目标（M3+ 计划接入 doctest）；当前以 `luban.exe doctor` + 手测覆盖

## 分发

- GitHub Releases：`luban.exe` + `luban-shim.exe` + `SHA256SUMS`
- CI：`.github/workflows/build.yml`（push/tag → win 构建 + artifact + 自动发版）
- 文档：`docs.yml` workflow 推送 mdBook + Doxygen 到 `gh-pages`（`force_orphan: true`）
- 域名 `luban.coh1e.com`（DNS-only Cloudflare，让 Let's Encrypt 能验证）
