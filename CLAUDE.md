# CLAUDE.md — luban project guide

> Hints for Claude Code (or any AI agent) working in this repo. Imperative,
> recipe-driven, terse. Design lives in `docs/DESIGN.md`; this file points
> at recipes and gotchas. Don't duplicate design rationale here.

## What luban is (15-second version)

**给一张图纸，搭一座 C++ 工坊。** Windows-first single-static-binary
(`luban.exe` ~5 MB, embeds Lua 5.4), zero UAC, XDG-first.

工坊三层：
- **承重墙**：C++ 工具链（llvm-mingw / cmake / ninja / vcpkg + opt. emscripten）
- **工作台**：C++ 工作流相关 CLI 利器（ripgrep / fd / bat / fzf / delta / gh / jq / eza / zoxide）
- **挂墙的图**：相关工具 dotfiles（`config` 块内置 5 个 renderer + `file` 块兜底）

按图纸叠层（先注册一个 bp source）：

```pwsh
luban bp src add Coh1e/luban-bps --name main   # 一次性，trust prompt
luban bp apply main/foundation                 # git+lfs+gcm+openssh（install.ps1 默认预装）
luban bp apply main/cpp-toolchain              # C++ 工具链（依赖 foundation）
luban bp apply main/cli-tools                  # 工作台 CLI（zoxide / starship / fd / ripgrep）
luban bp apply main/onboarding                 # 个人定制
```

luban.exe **零内嵌 bp**——议题 AG 终态（DESIGN §9.10）。基础 3 件
和个人 onboarding 都从 https://github.com/Coh1e/luban-bps 拉。
`embedded:<X>` 在 v1.0 已是硬错误（指向 `bp src add` 迁移路径）。

`luban.cmake` is git-tracked and is the only thing luban "owns" inside a
project — projects build on machines without luban installed.

## 概念：blueprint / tool / config

luban 的图纸描述"赋予一台机器某个能力"，自上而下三层：

- **blueprint（图纸 / 能力包）**：让机器获得某个能力所需的声明集合。
  `foundation` = git+lfs+gcm+openssh（必装，install.ps1 不询问预装）；
  `cpp-toolchain` = C++ 工具链；`cli-tools` = 工作台 CLI。
  一张图纸 ≈ 一种能力。所有 bp 来自外部
  bp source（`luban bp src add` 注册，§9.10）。
- **tool（工具 / 可执行物）**：实现能力靠的程序。一条 `tool` 声明对应一个
  PATH 上能调用的二进制（cmake / ninja / git / ssh / ...），由 luban 装在
  `~/.local/share/luban/store/` 并 shim 到 `~/.local/bin/`。
- **config（配置 / 设定）**：让 tool 按你的方式跑的 dotfile。一条 `config`
  声明喂给一个 renderer（`templates/programs/X.lua`），渲出
  `(target_path, content)`，drop-in 落到 XDG 路径。

关系：
- blueprint 是顶层容器；tool 和 config 是它的内容
- 同名的 `tool` 与 `config` 默认绑定同一个 X（如 `tool.bat` + `config.bat`）；
  显式绑定通过 `for_tool = "Y"` 覆盖（schema 化关系），缺省 = X
- 两边各有独立 lifecycle：cli-tools 可以只写 `config.git`（不装 git，
  由 foundation 装），或只写 `tool.gh`（不配 gh）。这是 Windows 现实——很多
  工具来自 Scoop / 系统 / 同事配的环境，luban 不必"也得装"才能配

> **schema 命名 in-flight**：上面用单数 `tool` / `config` 是已对齐的目标
> 命名。当前实现的 TOML 键还是历史的复数 `[tools.X]` / `[programs.X]`，
> 重命名到 `[tool.X]` / `[config.X]` 是单独 PR；本文档里 schema 示例代码段
> 仍按当前实现写，避免误导读者用未实现的语法。

## Verbs (34 入口)

详 `docs/DESIGN.md` §16. 按组：

- **blueprint** (11): `bp apply / unapply / ls / status / rollback / gc / search / src add / src rm / src ls / src update`（DESIGN §9.10 议题 AG, 2026-05-06；`bp src` 是 `bp source` 短别名）。`apply --update <bp>` 强制重 resolve（绕过本地 lock 缓存——v0.1.0/v0.1.1 用户从空 artifact_id 状态恢复必走）。所有 bp 来自外部 bp source（v1.0+ 零内嵌）。
- **project** (14): `new / build / check / test / doc / add / remove / target add+rm / tree / update / outdated / search / clean / fmt`
- **env** (2): `env / doctor`
- **utility** (5): `which / run / describe / self / completion`
- **transition** (1): `migrate` (v0.x bridge, 下个 release 删)

`luban describe` 支持 `port:<name>` / `tool:<name>` 前缀做 introspection（议题 AF）。

## Build (Windows, from a clean toolchain shell)

```bat
:: Activate luban-managed toolchain in the current shell:
luban env --user        :: once per machine; new shells just work
:: 或者临时：用现有 luban 的 shim PATH 拉起构建

cmake --preset default                  :: Debug
cmake --build --preset default

cmake --preset release                  :: Release
cmake --build --preset release

build\default\luban.exe --help          :: smoke
build\release\luban.exe --version       :: smoke
```

Unit tests (doctest, ADR-0004): `cmake --build --preset release --target luban-tests`
then `./build/release/luban-tests.exe`. Tests are EXCLUDE_FROM_ALL — default
`cmake --build` skips them.

End-to-end smoke: `scripts/smoke.bat` / `scripts/smoke.sh` runs new → add/remove →
build → run → target add/build → doctor without vcpkg network.

## Repo layout (essential)

```
src/                           # luban C++23 source
  main.cpp                     # registers every CLI verb — touch when adding one
  cli.{hpp,cpp}                # subcommand schema (flags/opts/forward_rest)
  paths.cpp                    # XDG-first 4-home + xdg_bin_home (~/.local/bin/)
  shim.cpp                     # text shim writer (.cmd) + table-backed collision checks
  shim_exe/main.cpp            # luban-shim.exe — separate binary used by .exe alias proxies
  win_path.cpp                 # HKCU PATH/env writeout
  commands/<verb>.cpp          # one cpp per verb
  util/win.hpp                 # utf8↔wstring conversions

  # Blueprint engine (§13.4):
  lua_engine.cpp               # Lua 5.4 VM + sandbox + luban.* API (v1 scaffold)
  qjs_engine.cpp               # QuickJS-NG 第二脚本层 (DESIGN §10)
  blueprint_{toml,lua}.cpp     # 图纸两种壳 (TOML 主, Lua inline 扩展) → BlueprintSpec
  blueprint_lock.cpp           # JSON lock R/W
  source_registry.cpp          # ~/.config/luban/sources.toml R/W (bp source 注册表)
  source_resolver.cpp          # tool source scheme 分发（github 半边在 _github.cpp）
  source_resolver_github.cpp   # GitHub releases API + asset 评分
  store{,_fetch}.cpp           # 内容寻址 store
  external_skip.cpp
  file_deploy.cpp              # replace / drop-in 部署 + backup
  config_renderer.cpp          # config 块 ([config.X]) 调度 + 5 个内置 renderer
  lua_json.cpp                 # lua table ↔ JSON 桥
  generation.cpp               # 叠层快照 + atomic rollback
  blueprint_apply.cpp          # 编排器
  commands/blueprint.cpp       # apply/unapply/ls/status/rollback/gc + sub-dispatch
  commands/bp_source.cpp       # bp src add/rm/ls/update + bp search (sub-verbs of bp)
  commands/migrate.cpp         # v0.x 桥（一次性）

  # Project / engineering 层:
  vcpkg_manifest.cpp           # 安全编辑 vcpkg.json
  lib_targets.cpp              # 表驱动：port → find_package + targets[]
  luban_cmake_gen.cpp          # 重渲项目内 luban.cmake
  luban_toml.cpp               # 解析 luban.toml ([project] kind / [ports.X] / 偏好)

                               # NOTE: templates/blueprints/ 已移除（议题 AG）。
                               # 基础 3 件 + onboarding 现在住外部 bp source repo
                               # https://github.com/Coh1e/luban-bps；luban.exe 零内嵌。
templates/programs/            # 内置 5 个 config renderer（Lua）；目录名沿用
  git.lua / bat.lua / fastfetch.lua / yazi.lua / delta.lua
                               # 计划随 schema 重命名一并改成 templates/configs/
templates/{app,lib,wasm-app}/  # luban new <kind> 脚手架
                               # `{{name}}` 在内容 + 目录名都展开

third_party/                   # 单 header vendored libs + lua54/
  json.hpp miniz.{h,c} toml.hpp doctest.h + their LICENSEs
  lua54/                       # 60 .c/.h（破例多文件）
docs/DESIGN.md                 # 唯一设计文档
templates/help/                # 长 --help 文本，编进二进制
.github/workflows/build.yml    # windows MSVC + MinGW dual-build, tag→release
```

## Don't break these (8 不变量；详 DESIGN.md §6)

1. **cmake 仍是主体**——不发明 IR、不替换 manifest
2. **`luban.cmake` schema 稳定**——已在野的项目 git-tracked
3. **`vcpkg.json` 是项目依赖唯一真相**——`luban.toml` 不加 `[deps]`
4. **`luban.toml` 可选**——只载 `[project] kind` / `[scaffold]` / `[ports.X]` / 偏好
5. **XDG-first**——bin shim 走 `~/.local/bin/`，toolchain bin **绝不进 PATH**（议题 G）
6. **零 UAC、HKCU only**
7. **`luban.exe` 必须 static-linked**——fresh Win10 无 PATH 即可跑
8. **toolchain ≠ 项目库**——`luban add cmake` 必须被拒绝

**放宽**：第三方 vendor 优先 single-header；Lua 5.4 与 QuickJS-NG 是
当前两个多文件例外（Lua 主、QJS 副，DESIGN §10）。

## Common task recipes

### Add a new CLI verb `foo`

1. `src/commands/foo.cpp` — `int run_foo(const cli::ParsedArgs&)` + `void register_foo()`.
   Set `c.group` (`blueprint` / `project` / `env` / `utility` / `transition`) +
   `c.long_help` + `c.examples`.
2. `CMakeLists.txt` — append `src/commands/foo.cpp` to `LUBAN_SOURCES`.
3. `src/main.cpp` — declare + call `register_foo()`.
4. (Optional) `templates/help/foo.md` + 加进 `embed_text.cmake` 的 foreach。

### Add a vcpkg port → cmake target mapping

Edit `src/lib_targets.cpp` table. Each row: `{port, find_package_name,
{target1, ...}}`. 用户私有 port 走 `luban.toml [ports.X]`（议题 AB），
不进这表。

### Add a tool to a blueprint (in your bp source repo)

luban.exe **零内嵌 bp**——基础 3 件 + onboarding 都在
https://github.com/Coh1e/luban-bps。改图纸 = 改外部 repo，跟 luban.exe
版本解耦。

1. 选层：网络/git 通用工具进 `foundation.toml`；C++ 编译相关进 `cpp-toolchain.toml`；
   日常 CLI 利器进 `cli-tools.toml`。新单工具图纸 = 新 .toml 在你 bp
   source repo 的 `blueprints/` 下（**luban 这边不动代码**）。
2. 在 `[tools.X]` 节加 `source = "github:owner/repo"`；非 GitHub releases 用
   显式的 `[[tools.X.platform]]` block（url + sha256 + bin）。多 binary 工具
   有两种声明方式：(a) `shims = ["bin/a.exe", "bin/b.exe"]` 显式列入口；
   (b) `shim_dir = "bin"` 让 luban 自动 shim 该目录下所有 `.exe`（v0.1.6+，
   适合 llvm-mingw 这种 ~270 binary 的工具，避免硬编码 list 跟上游版本漂移）。
   两个字段可以并存——`shims` 优先级高，`shim_dir` 跳过同名 alias。
   本机已装就跳过的工具加 `external_skip = "ssh.exe"`（probe 名跟 tool 名
   不一致时用）。
3. 添加新 source scheme：照 `src/source_resolver_github.cpp` 写一个 per-scheme TU，
   在 `LUBAN_SOURCES` 里加一行，static-init 注册到 `source_resolver` 的 dispatch table
   （blueprint Lua 直接 register 是 v1.1 工作）。
4. `post_install = "<rel-path>"` 字段在 `[tools.X]`，extract 后跑一次性
   脚本（vcpkg bootstrap 风格）。脚本路径相对 artifact 根，路径穿越被
   blueprint_apply 拦截；Windows 走 `cmd /c`，POSIX 走 `/bin/sh`。
   仅在新提取（非缓存命中）时触发。
5. push bp source repo 后，`luban bp src update <name>` 让本机拉新内容。

### Add a new built-in config renderer

`templates/programs/<tool>.lua` — 模块返回 `{ render(cfg, ctx), target_path(cfg, ctx) }`。
然后 `CMakeLists.txt` 的 `foreach(PROG IN ITEMS ...)` 加上 `<tool>`，
并在 `src/config_renderer.cpp` `#include` 生成的 header + 加进 switch。
图纸里用 `[programs.<tool>]` 块喂 cfg。
（用户级 renderer 走 `luban.register_renderer` Lua API 是 v1.1 工作；
详 docs/FUTURE.md。）

### Cut a release vX.Y.Z

```bat
:: bump version (one line, one file)
sed -i "s|VERSION [0-9.]*|VERSION X.Y.Z|" CMakeLists.txt
:: build + smoke locally (whichever toolchain you have on hand)
cmake --build --preset release
build\release\luban.exe --version

:: tag + push — CI does the dual-flavor release
git commit -am "Bump X.Y.Z" && git push
git tag -a vX.Y.Z -m "luban X.Y.Z — ..." && git push --tags
```

**Release flavors (议题 R, 2026-05-06)**: every release ships TWO
Windows binaries, both static-linked (invariant 7 holds for either).
`.github/workflows/build.yml` runs the two builds in parallel and a
third job stitches them into a single GitHub release.

| asset                       | toolchain              | size  | when to pick |
|-----------------------------|------------------------|-------|--------------|
| `luban-msvc.exe`            | windows-latest cl /MT  | ~3 MB | **default**. Smaller binary; fastest startup; no MinGW dependency on the dev box. |
| `luban-shim-msvc.exe`       | -                      | ~150 KB | shim twin for MSVC luban |
| `luban-mingw.exe`           | llvm-mingw 20260421 -static | ~6 MB | legacy / portability fallback. Same toolchain `cpp-toolchain` installs, so dev/release are bit-identical. |
| `luban-shim-mingw.exe`      | -                      | ~600 KB | shim twin for MinGW luban |
| `SHA256SUMS`                | -                      | -     | covers all 4 binaries |

`install.ps1` defaults to msvc; users who want the MinGW flavor set
`$env:LUBAN_FLAVOR='mingw'` before invoking. On disk both flavors land
as `luban.exe` / `luban-shim.exe` (suffix only lives in the release
asset name) so downstream `.cmd` / `.exe` PATH probes resolve uniformly.

CI verifies invariant 7 on both flavors:
- MSVC: `dumpbin /DEPENDENTS` rejects vcruntime / msvcp / api-ms-win-crt
- MinGW: `llvm-readobj --coff-imports` rejects libgcc_s / libstdc++ /
  libc++ / vcruntime / msvcp

## User-facing env vars

调用方常用，install.ps1 + luban C++ 都识别：

| env | 默认 | 说明 |
|---|---|---|
| `LUBAN_INSTALL_DIR` | `~/.local/bin` | install.ps1 安装目标目录 |
| `LUBAN_FORCE_REINSTALL` | unset | =1 时 install.ps1 跳过 SHA-命中短路 |
| `LUBAN_FLAVOR` | `msvc` | release 选哪个 flavor (`msvc` \| `mingw`) |
| `LUBAN_GITHUB_MIRROR_PREFIX` | unset | 反代 prefix（如 `https://ghfast.top`），重写 `github.com` / `*.githubusercontent.com` URL；**不**重写 `api.github.com`（公共 mirror 都 403 它）。注意 ghfast.top 限速严格，VN 网络下直连可能更快 |
| `LUBAN_PARALLEL_CHUNKS` | 1 | bp apply 下载时 Range 并发数（0 / 1 = 单流，最高 16）。**默认单流是有原因的**——GitHub release CDN 对多并发 per-IP throttle 很狠，VN 网络实测 1 路 4.7 MB/s，4 路掉到 150 KB/s。只在私有 S3 / 内网镜像这种不限速的场景调高（详 DESIGN §25.2） |
| `LUBAN_ENABLE_HTTP2` | unset | =1 让 WinHTTP 协商 HTTP/2。**默认 H1.1**（v0.2.6 翻），因为 release CDN（Fastly 托管的 objects.githubusercontent.com）对 VN 客户端 H2 慢得离谱（实测 14.9 KiB/s 对 H1.1 47 MiB/s，~3200× 差）。如果你网络下 H2 更快才开 |
| `LUBAN_ENABLE_HTTP3` | unset | =1 在已开 H2 基础上再打开 H3 (QUIC over UDP/443)。**默认关**——VN/CN 网络下 UDP/443 被节流时 H3 协商超时 ~10s 才回退 H2，体感巨慢 |
| `LUBAN_EXTRACT_THREADS` | min(8, hw_concurrency) | archive::extract worker 数（0 = 单线程）。llvm-mingw 测 4 路对单流 ~5× 加速，超过 ~8 SSD 写饱和无意义 |
| `LUBAN_PROGRESS` | unset | =1 强制开 progress bar（非 TTY 也开） |
| `LUBAN_NO_PROGRESS` | unset | =1 关 progress bar |

## Known quirks

- **D:\dobby junction**：`C:\Users\Rust\.local\share\luban` 与
  `D:\dobby\.local\share\luban` 指向同一文件。**不要** `canonical()`/
  `weakly_canonical()`——可能跨 junction 解析破坏 `relative_to()`。
- **gh CLI workflow scope**：推 `.github/workflows/*` 需 token 有 `workflow`
  scope。被拒就 `gh auth refresh -s workflow`。
- **Tests 用 vendored doctest** (ADR-0004)：每 leaf 模块一个
  `tests/test_<module>.cpp`，加进 `LUBAN_BUILD_TESTS` block。
- **shim 路径** = `~/.local/bin/`（不变量 5）。任何代码写老 `<data>/bin/`
  都是 v0.x 残留，应改走 `paths::xdg_bin_home()`。
- **空 artifact_id 的 stale lock**（v0.1.0 / v0.1.1 遗留）：早期 resolver
  把 `lp.artifact_id = ""` 写进 lock 期望 store 填——但 store 用值不计算。
  结果 `final_dir = <store>/`（store 根本身），`fs::rename(tmp, store_root)`
  以 ERROR_PATH_NOT_FOUND 失败，cache 里还堆个 `.archive` 孤儿文件。
  v0.1.2+ resolver 在 `lp.sha256` 拿到后调 `compute_artifact_id` 写进 lock；
  store::fetch 也 guard 空 id。**用户从 v0.1.0/0.1.1 升级后必须**
  `luban bp apply --update <bp>` 一次重写 lock，否则继续命中老 lock 报错。
- **GitHub mirror 限速**（ghfast.top / gh-proxy.com 等公共反代）：免费 IP 限速
  到几 KB/s 是常态。VN/CN 网络如果直连 github.com 可达就**别**走 mirror——
  实测直连可比 mirror 快 25×。mirror 的真正用武之地是 GitHub 完全封堵
  的网络。

## Where to read more

- **Design**: `docs/DESIGN.md` — 唯一设计文档；架构 / 名词 / 动词 / 决议
  全在里面。§24.1 是字母序的决议表（A → AB），可以当索引。
- **History**: `git log --oneline` — 每步独立 commit。

## Critical reusable functions (don't reinvent)

| Function | File | Use |
|---|---|---|
| `paths::{data,config,cache,state}_dir()` | `src/paths.cpp` | XDG-resolved 4 homes |
| `paths::xdg_bin_home()` | `src/paths.cpp` | `~/.local/bin/`（不变量 5）|
| `proc::run(cmd, cwd, env_overrides)` | `src/proc.cpp` | spawn with env merge |
| `download::download(url, dest, opts)` | `src/download.cpp` | HTTPS GET + SHA |
| `archive::extract(zip, dest)` | `src/archive.cpp` | ZIP w/ traversal guard |
| `hash::verify_file(path, spec)` | `src/hash.cpp` | SHA256 校验 |
| `env_snapshot::apply_to(env)` | `src/env_snapshot.cpp` | PATH + persistent_env + injected_env overlay |
| `luban_cmake_gen::regenerate_in_project(dir, targets)` | `src/luban_cmake_gen.cpp` | 重渲项目 `luban.cmake` |
| `vcpkg_manifest::{add,remove,save}` | `src/vcpkg_manifest.cpp` | 安全编辑 `vcpkg.json` |
| `lua_engine::Engine::eval_*` | `src/lua_engine.cpp` | Sandboxed Lua VM (v1 scaffold) |
| `config_renderer::render(name, cfg, ctx)` | `src/config_renderer.cpp` | config 块 → renderer 调度（5 个内置） |
| `cli::Subcommand::forward_rest` | `src/cli.hpp` | argv 透传（`luban run` 风格）|
