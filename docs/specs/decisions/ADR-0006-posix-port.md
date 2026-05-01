# ADR-0006 — Linux/macOS port: scope, gates, and current stub state

- **状态**：Proposed (M4 target)
- **日期**：2026-04-30
- **作者**：Coh1e + Claude
- **取代**：n/a
- **关联 OQ**：OQ-4 (Linux/macOS 移植)

## Context

luban v0.x 完全是 Windows-first：vcpkg + LLVM-MinGW + WinHttp + BCrypt + HKCU 全
栈围着 Win10/11 转。代码层面 19 个 `.cpp` 文件用 `#ifdef _WIN32` 隔离平台代码；
其中 16 个已经写了 `#else` 分支或用空实现兜底（`hash::hash_file` 在 POSIX 返回
`nullopt`、`download::do_request` 返回 `"POSIX download not implemented"` 错误），
**架构层面 cross-platform 已经成型**。但这远不等于"在 Linux 上能 build 并跑通核心
路径"。本 ADR 把当前缺口、补全顺序和 gating 写明，给 M4 冲刺前的工程留个 kickoff 文档。

## Current state — what compiles vs. what works

### 已经在源码中分平台的模块

| 模块 | Windows 实现 | POSIX 现状 |
|---|---|---|
| `paths.cpp` | `SHGetKnownFolderPath` + `LOCALAPPDATA` | ✅ 完整 — `XDG_*` + `~/.local/...` 走的就是 POSIX 分支 |
| `proc.cpp` | `CreateProcessW` | ✅ 完整 — `fork + execvp + waitpid` |
| `log.cpp` | `GetStdHandle + SetConsoleMode`（VT enable） | ✅ POSIX 默认 ANSI 终端，无需额外初始化 |
| `hash.cpp` | BCrypt | ⚠️ stub — 返回 `nullopt`；下载哈希校验全失效 |
| `download.cpp` | WinHttp | ⚠️ stub — 返回 `"POSIX download not implemented"` |
| `win_path.cpp` | HKCU regedit | ⚠️ stub — 应当改写 `~/.profile` / `~/.bashrc` |
| `msvc_env.cpp` | vswhere + vcvarsall | ✅ 平台不相关时 no-op（POSIX 无 MSVC 概念） |
| `perception.cpp` | `__cpuid` + `GlobalMemoryStatusEx` | ✅ 已有 `/proc/cpuinfo` + `sysconf` 实现 |
| `archive.cpp` | miniz 单 header | ✅ 平台无关 |
| `shim_exe/main.cpp` | `CreateProcessW` 代理 | ❌ 不需要 — POSIX `~/.local/bin` symlink 直接走 PATH，本 ADR 提交把 luban-shim 包进 `if(WIN32)`，POSIX 构建直接跳过 |

### Build 系统

CMakeLists.txt 已经正确 gate 了 Windows-only link libs（`shell32 ole32 advapi32 user32 bcrypt winhttp`），它们都在 `if(WIN32)` 块里。POSIX 构建跳过这些。但是**没有等价的 POSIX link libs**（pthread / dl / libcurl / openssl），因为对应的功能还是 stub。

### CI

`.github/workflows/build.yml` 只跑 `windows-latest` runner。M4 启动后需要加 `ubuntu-latest` + `macos-latest` matrix。

## Decision — staged port plan

把 POSIX 移植分三个 phase，每个 phase 独立可发布：

### Phase A — luban builds clean on Linux/macOS（M4 entry gate）

不要求功能完整，只要求 `cmake --preset default && cmake --build` 在 Ubuntu 24.04 LTS 和 macOS 14 都能跑过去。**stub 仍然是 stub**，但二进制能链接、`luban --version` 能跑、`luban doctor` 能给出诚实的不可用诊断。

实现：
1. 加 ubuntu/macos CI runner（仅做 build 验证，跳过所有 install-flow 烟测）
2. 加 `if(NOT WIN32) target_link_libraries(luban PRIVATE pthread)` 兜 `<thread>`
3. 把 `clang --version` 探测加入 doctor，POSIX 上代替 LLVM-MinGW 检查
4. doctor 在 POSIX 输出 "download/hash 子系统在本平台未实现 — Phase B 解锁" 这种诚实诊断

### Phase B — 关键路径可运行（download + hash 实装）

luban 的所有命令都依赖下载（component install）+ 哈希校验。Phase B 只解这两个：

1. **hash.cpp POSIX 分支**：用 OpenSSL `EVP_DigestInit_ex / EVP_DigestUpdate / EVP_DigestFinal_ex`。OpenSSL 在 Linux 上是系统库（`libssl-dev`），macOS 上 Homebrew + `pkg-config` 解析。用 `find_package(OpenSSL REQUIRED)` 在 POSIX 分支；ADR-0001 invariant 1（"single-header vendor"）的 escape hatch 是"系统库"——OpenSSL 是 Linux 发行版必有的，不构成"luban 引第三方依赖"。
2. **download.cpp POSIX 分支**：用 libcurl `easy_init / easy_setopt / easy_perform`。同样系统库（`libcurl4-openssl-dev`）。

实装后 `luban setup --with cmake` 在 Linux 上能真下能装。

### Phase C — toolchain/scaffolding 适配 POSIX 习惯

Phase B 之后所有"网络 + 文件"已经通了，剩下的是平台习惯：

1. **win_path.cpp POSIX 分支**：`luban env --user` 应当往 `~/.bashrc` / `~/.zshrc` / `~/.config/fish/config.fish` 追加 `export PATH=...`。检测 `$SHELL` 决定写哪个。**比 Windows 更复杂**因为 shell 多样性 —— Phase C.1 只支持 bash + zsh + fish。
2. **shim 模型**：POSIX 上不需要 `.cmd` / `.exe` 双写，直接 symlink `<data>/bin/cmake → <toolchain>/cmake-X.Y.Z/bin/cmake`。`shim.cpp` 的 POSIX 分支用 `fs::create_symlink`（已经存在的 stub 应改成实装）。
3. **luban.toml [project] triplet 默认值**：当前硬编码 `x64-mingw-static`。POSIX 上 vcpkg triplet 应当跟随平台 —— Linux `x64-linux`、macOS `arm64-osx` / `x64-osx`。`paths.cpp` 加一个 `default_triplet()` helper。
4. **manifests_seed/* 的 url**：当前只列 Windows 二进制。POSIX 需要平台对应的 release 资产。每个 manifest 加 `windows / linux / macos` 字段，按 `paths::os_id()` 选取。
5. **install.sh**：现在仓库根有 `install.ps1` 占位 `install.sh`；Phase C 把它实装成"下载 luban 到 `~/.local/bin/luban`，建议跑 `luban env --user`"。

### Out of scope — Phase D+

- **MSVC 跨平台**：POSIX 上 MSVC 不存在；`luban setup --with msvc` 在 POSIX 直接报错"Windows-only component"
- **Cross-target build**：`luban build --target=linux-x86_64` 在 macOS 跑 zig cc 跨编 — 看 OQ 列表里"Cross-target build"那条，独立设计

## Consequences

**Pros**：
- 已经有了平台分层的代码结构 — Phase A 几乎零工作量（只加 CI runner）
- `hash + download` 改 OpenSSL/libcurl 是大概 200 行新代码，单元测试已经能覆盖（`tests/test_hash.cpp` 是平台无关的）
- 用户可以在每个 phase 之后立即用上 luban 的部分功能

**Cons / 风险**：
- OpenSSL + libcurl 的版本断点散布在不同发行版里（Ubuntu 22.04 OpenSSL 3 / Ubuntu 20.04 OpenSSL 1.1 / macOS 自带 LibreSSL）— Phase B 要写出兼容矩阵
- POSIX shell 多样性比想象中大；Phase C.1 限定 bash/zsh/fish 是合理切线，但还是要应对用户用 `/bin/sh` 的情况
- macOS Homebrew 用户和 Linux 包管理用户的混用会让"找 OpenSSL 头"这件事在 cmake 里不稳定 — 可能要 fallback 到 vendored libcrypto subset（mbedTLS 单 header 可能比 OpenSSL 更现实）

## Alternatives considered

- **vendored mbedTLS / mbedcrypto** 替代 OpenSSL：mbedTLS ~5k 行，可单 header 化但需要工作；优点是不依赖系统库，缺点是更新滞后
- **vendored cpp-httplib + bearssl**：cpp-httplib 是单 header 但只是 HTTP 层，TLS 还要补；bearssl 不是 single-header
- **rust crate via FFI**：跟 ADR-0002 scope (1)（luban.exe 不引 Rust 运行时依赖）冲突，**否决**
- **Linux 用 `wget`/`curl` 子进程下**：unhealthy — 让 luban 依赖外部 PATH 工具不可控（Container 镜像可能没 curl）

## Out-of-scope — 留给后续 ADR

- ADR-XXXX：POSIX shim 模型详细设计（symlink vs `#!/bin/sh` 包装）
- ADR-XXXX：vcpkg triplet 自动选取规则
- ADR-XXXX：`install.sh` 与 `install.ps1` 的契约（必须满足相同的输出格式给 doctor 报告）

---

**当前 PR 的提交内容**：
- `if(WIN32)` 包住 luban-shim.exe target —— POSIX 构建跳过
- 本 ADR 文件
- 不动现有 `_WIN32 / #else` stub 分支（它们已经够 honest）
