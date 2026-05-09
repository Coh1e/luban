# CLAUDE.md — luban project guide

> Hints for Claude Code (or any AI agent) working in this repo. Imperative,
> recipe-driven, terse. Design lives in `docs/DESIGN.md`; this file points
> at recipes and gotchas. Don't duplicate design rationale here.

## v1.0.0 — MVP convergence shipped

luban v1.0.0 is the DESIGN §1–§11 MVP. The verb surface, module
inventory, and blueprint frontend all now match the doc. Notable
differences from earlier prototypes:

- **Lua is the only blueprint format** (DESIGN §2 #6). `.toml` blueprints
  are not parsed; only `.lua` is accepted.
- **No generation/rollback** (DESIGN §11). State persists as flat files
  in `<state>/luban/{applied.txt,owned-shims.txt}` via `applied_db`.
- **No tool install registry** (DESIGN §11 drops the v0.x model).
  `installed.json` is gone; `bp source` registry + `applied.txt` is the
  authoritative successor.
- **No QuickJS-NG** (DESIGN §2 #6 picks Lua as the single script layer).
- **Only `github:` source scheme** (DESIGN §11 drops generic application
  install — no more `pwsh-module:`).
- **`msvc shell` / `msvc run -- <cmd>` is the canonical MSVC entry point**
  (DESIGN §6.2). `env --msvc-init` / `--msvc-clear` survive as the
  internal capture mechanism.

## What luban is (15-second version)

**给一张图纸，搭一座 C++ 工坊。** Windows-first single-static-binary
(`luban.exe` ~5 MB, embeds Lua 5.4), zero UAC, XDG-first.

工坊三层（DESIGN §3.1）：

- **bootstrap**：git / mingit / gcm（fresh host 第一刚需）
- **toolchain**：llvm-mingw / cmake / ninja / vcpkg（C++ 编译能力）
- **workbench**：fd / ripgrep / PowerShell 7 / PSReadLine / zoxide / starship /
  Windows Terminal / 字体与主题（日常 CLI 体验）

按图纸叠层（先注册一个 bp source）：

```pwsh
luban bp source add Coh1e/luban-bps --name main   # 一次性，trust prompt
luban bp apply main/foundation                    # bootstrap 层
luban bp apply main/cpp-toolchain                 # toolchain 层
luban bp apply main/cli-tools                     # workbench 层
luban bp apply main/onboarding                    # 个人定制
```

luban.exe **零内嵌 bp**。所有 bp 来自外部 bp source repo
（`Coh1e/luban-bps`）。`luban.cmake` is git-tracked and is the only thing
luban "owns" inside a project — projects build on machines without luban
installed.

## 概念：blueprint / tool / config（DESIGN §4）

luban 的图纸描述"赋予一台机器某个能力"，自上而下三层：

- **blueprint（图纸 / 能力包）**：一张远端 Lua 图纸，描述让机器获得某个能力
  所需的声明集合。`foundation` = git+gcm；`cpp-toolchain` = C++ 工具链；
  `cli-tools` = 工作台 CLI。一张图纸 ≈ 一种能力。**DESIGN §2 #6: Lua DSL
  是 blueprint 的唯一入口**，TOML 仅作为可选的静态 projection（describe 输出
  方向，不是输入方向）。
- **tool（工具 / 可执行物）**：实现能力靠的程序。一条 `tool` 声明对应一个
  PATH 上能调用的二进制（cmake / ninja / git / ssh / ...），由 luban 装在
  `~/.local/share/luban/store/` 并 shim 到 `~/.local/bin/`。
- **config（配置 / 设定）**：让 tool 按你的方式跑的 dotfile。一条 `config`
  声明喂给一个 renderer（`templates/configs/X.lua`），渲出
  `(target_path, content)`，drop-in 落到 XDG 路径。Renderer 必须声明
  capability（DESIGN §4 Config）：可写哪些目录、是否覆盖、是否需要确认。

关系：

- blueprint 是顶层容器；tool 和 config 是它的内容
- 同名的 `tool` 与 `config` 默认绑定同一个 X（如 `tool.bat` + `config.bat`）；
  显式绑定通过 `for_tool = "Y"` 覆盖（schema 化关系），缺省 = X
- 两边各有独立 lifecycle：cli-tools 可以只写 `config.git`（不装 git，
  由 foundation 装），或只写 `tool.gh`（不配 gh）

> **schema 命名**：单数 `[tool.X]` / `[config.X]` / `[file."path"]`（Lua
> 形式同义：`tools = { X = {...} }` / `configs = {...}` / `files = {...}`）。
> `for_tool = "Y"` 可显式覆盖隐式同名绑定。

## 信任模型（DESIGN §8）

- 官方 bp source（默认信任）：apply 前仍展示 trust summary
- 非官方 bp source：红色警告 + 显式来源 + capability 列表 + 用户确认
- doctor 必须能指出：bp/tool 来源非官方、tool 走 TOFU、tool 来自 external skip

## Verbs（DESIGN §5 MVP = 13 user-facing, 12 register）

| group | verb |
| --- | --- |
| blueprint | `bp source add` / `bp source update` / `bp list` / `bp apply` (+ `--dry-run`) |
| project | `new` / `add` / `remove` / `build` / `run` |
| env | `env` / `doctor` |
| utility | `describe` / `self update` / `self uninstall` (+ `--dry-run`) |
| msvc | `msvc shell` / `msvc run -- <cmd>` (DESIGN §6.2) |

`luban describe` 支持 `port:<name>` / `tool:<name>` 前缀做 introspection。

实际 12 个 `register_*()` 调用见 `src/main.cpp`（doctor / env / new / build /
add / remove / run / describe / self / blueprint / msvc + the bp dispatch
which serves source/list/apply sub-verbs internally).

## Build (Windows, from a clean toolchain shell)

```bat
:: Activate luban-managed toolchain in the current shell:
luban env --user        :: once per machine; new shells just work

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
  log.cpp                      # logging
  shim.cpp                     # text shim writer (.cmd) + table-backed collision checks
  shim_exe/main.cpp            # luban-shim.exe — separate binary used by .exe alias proxies
  xdg_shim.cpp                 # bin shim install/remove (XDG home)
  win_path.cpp                 # HKCU PATH/env writeout
  proc.cpp                     # spawn with env merge
  hash.cpp                     # SHA256 verify
  download.cpp                 # HTTPS GET + SHA (Win → curl_subprocess; POSIX → libcurl)
  curl_subprocess.cpp          # Win32 curl.exe subprocess driver
  archive.cpp                  # ZIP extract w/ traversal guard
  progress.{hpp,cpp}           # 统一进度 UI
  perception.cpp               # introspection helpers
  msvc_env.cpp                 # vcvarsall env capture
  file_util.cpp                # file utilities
  path_search.cpp              # PATH search
  platform.cpp                 # OS abstraction
  env_snapshot.cpp             # PATH + injected_env overlay (xdg_bin_home + msvc dirs)
  external_skip.cpp            # external_skip = "ssh.exe" probing
  applied_db.cpp               # <state>/luban/{applied.txt,owned-shims.txt}
  iso_time.cpp                 # ISO-8601 UTC timestamp helper
  commands/<verb>.cpp          # one cpp per verb

  # Blueprint engine:
  lua_engine.cpp               # Lua 5.4 VM + sandbox + luban.* API
  lua_engine_download.cpp      # download() exposed to Lua sandbox
  lua_frontend.cpp             # 唯一允许 #include <lua.h> 的 .cpp（不变量 9）
                               # 把 lua refs / embedded module 包成 std::function 喂 core
  lua_json.cpp                 # lua table ↔ JSON 桥
  blueprint_lua.cpp            # 唯一 blueprint frontend (DESIGN §2 #6 / §4)
  blueprint_lock.cpp           # JSON lock R/W (内部，DESIGN §4 不暴露)
  source_registry.cpp          # ~/.config/luban/sources.toml R/W
  source_resolver.cpp          # source 块 → registry-first → C++ scheme dispatch
  source_resolver_github.cpp   # 唯一 source scheme (DESIGN §11 drops 其他)
  store.cpp / store_fetch.cpp  # 内容寻址 store
  file_deploy.cpp              # replace / drop-in / append 部署 + backup
  config_renderer.cpp          # config 块 → renderer dispatch (core 不引 lua.h)
  renderer_registry.cpp        # name → RendererFns (std::function)
  resolver_registry.cpp        # scheme → ResolverFn (std::function)
  blueprint_apply.cpp          # 编排器（applied_db + owned-shims 落盘）
  commands/blueprint.cpp       # apply/list/source 子派发
  commands/bp_source.cpp       # bp source add/update sub-verbs
  commands/msvc.cpp            # msvc shell / msvc run -- <cmd>

  # Project / engineering 层:
  vcpkg_manifest.cpp           # 安全编辑 vcpkg.json (TOML — 项目层，与 blueprint 无关)
  lib_targets.cpp              # 表驱动：port → find_package + targets[]
  luban_cmake_gen.cpp          # 重渲项目内 luban.cmake
  luban_toml.cpp               # 解析 luban.toml ([project] kind / [ports.X] / 偏好)

templates/configs/             # 内置 5 个 config renderer (Lua)
  git.lua / bat.lua / fastfetch.lua / yazi.lua / delta.lua
templates/{app,wasm-app}/      # luban new <kind> 脚手架
                               # `{{name}}` 在内容 + 目录名都展开
templates/help/                # 长 --help 文本，编进二进制（目前只有 new.md）

third_party/                   # 单 header vendored libs + lua54/
  json.hpp miniz.{h,c} toml.hpp doctest.h + their LICENSEs
  lua54/                       # 60 .c/.h（唯一多文件破例）

docs/DESIGN.md                 # 唯一设计文档 (§1–§11 MVP requirements)
.github/workflows/build.yml    # single windows MSVC build, tag→release
```

## Don't break these (不变量；详 DESIGN §2 北极星)

1. **CMake 是主体**（DESIGN §2 #1）—不发明 IR、不替换 manifest
2. **`luban.cmake` schema 稳定**——已在野的项目 git-tracked
3. **`vcpkg.json` / FetchContent 是项目依赖唯一真相**（DESIGN §2 #2）—`luban.toml` 不加 `[deps]`
4. **`luban.toml` 可选**——只载 `[project] kind` / `[scaffold]` / `[ports.X]` / 偏好
5. **XDG-first**——bin shim 走 `~/.local/bin/`，toolchain bin **绝不进 PATH**（DESIGN §2 #5）
6. **零 UAC、HKCU only**（DESIGN §2 #4）
7. **`luban.exe` 必须 static-linked**——fresh Win10 无 PATH 即可跑
8. **toolchain ≠ 项目库**（DESIGN §2 #3）——`luban add cmake` 必须被拒绝
9. **core C++ 不引 `lua.h`**——`renderer_registry` / `resolver_registry` /
   `config_renderer` / `source_resolver` / `blueprint_apply` 这 5 个 core
   模块只看到 `std::function<...>`；任何 lua C API 调用集中在
   `src/lua_frontend.cpp` 一个 TU
10. **每次 apply 都构造 Engine + 两 registries**——所有 bp 走 Lua engine（v1.0
    后唯一 frontend），5 个内置 renderer 也注册成 `std::function` 进 registry。
    builtin / bp-registered / 未来 native plugin 在 dispatch 上无差别——
    "无双码路径"
11. **luban 是便利层，不接管 CMake**（DESIGN §2 #8）——生成的 CMake 内容
    脱离 luban 仍可用

**放宽**：第三方 vendor 优先 single-header；Lua 5.4 是唯一多文件例外。

## Common task recipes

### Add a new CLI verb `foo`

1. `src/commands/foo.cpp` — `int run_foo(const cli::ParsedArgs&)` + `void register_foo()`.
   Set `c.group` (`blueprint` / `project` / `env` / `utility`) +
   `c.long_help` + `c.examples`.
2. `CMakeLists.txt` — append `src/commands/foo.cpp` to `LUBAN_SOURCES`.
3. `src/main.cpp` — declare + call `register_foo()`.
4. (Optional) `templates/help/foo.md` + 加进 `embed_text.cmake` 的 foreach。
   目前只有 `new` 走 embedded help；其他 verb 用 inline `c.long_help`。

### Add a vcpkg port → cmake target mapping

Edit `src/lib_targets.cpp` table. Each row: `{port, find_package_name,
{target1, ...}}`. 用户私有 port 走 `luban.toml [ports.X]`，不进这表。

### Add a tool to a blueprint (in your bp source repo)

luban.exe **零内嵌 bp**——基础 3 件 + onboarding 都在
https://github.com/Coh1e/luban-bps。改图纸 = 改外部 repo，跟 luban.exe
版本解耦。**v1.0.0 后只接受 .lua 形式**：

```lua
-- blueprints/cli-tools.lua
return {
  schema = 1,
  name = "cli-tools",
  description = "...",
  tools = {
    zoxide = { source = "github:ajeetdsouza/zoxide" },
    fd = { source = "github:sharkdp/fd" },
  },
  configs = { ... },
  files = {
    ["~/.config/ripgrep/ripgreprc"] = { mode = "replace", content = "..." },
  },
  meta = { requires = {}, conflicts = {} },
}
```

字段说明：

- `source = "github:owner/repo"` 是默认 GitHub releases 抽取。非 GitHub 走
  显式 `platform = { { url=..., sha256=..., bin=... } }` block。
- 多 binary 工具：`shims = { "bin/a.exe", "bin/b.exe" }` 显式列；或
  `shim_dir = "bin"` 让 luban 自动 shim 该目录下所有 `.exe`（适合 llvm-mingw
  这种 ~270 binary 的工具）。两个字段可以并存——`shims` 优先级高。
- 本机已装就跳过的工具加 `external_skip = "ssh.exe"`（probe 名跟 tool 名
  不一致时用）。
- `post_install = "<rel-path>"` 在 `tool.X`，extract 后跑一次性脚本
  （vcpkg bootstrap 风格）。路径相对 artifact 根，路径穿越被
  blueprint_apply 拦截；Windows 走 `cmd /c`，POSIX 走 `/bin/sh`。
  仅在新提取（非缓存命中）时触发。
- 添加新 source scheme：照 `src/source_resolver_github.cpp` 写一个 per-scheme
  TU，在 `LUBAN_SOURCES` 加一行，static-init 注册到 `source_resolver` 的
  dispatch table（blueprint Lua 直接 `register_resolver` 是后续工作）。

push bp source repo 后，`luban bp source update <name>` 让本机拉新内容。

### Add a new built-in config renderer

1. `templates/configs/<tool>.lua` — 模块返回 `{ target_path = fn(cfg, ctx),
   render = fn(cfg, ctx) }`。
2. `CMakeLists.txt` 的 `foreach(CFG IN ITEMS ...)` 列里加 `<tool>` —— embed
   step 会生成 `gen/luban/embedded_configs/<tool>.hpp` 含
   `embedded_configs::<tool>_lua` 字符串。
3. `src/commands/blueprint.cpp` 顶端 `#include "luban/embedded_configs/<tool>.hpp"`
   并往 `kBuiltinRenderers[]` 表加一行 `{"<tool>", luban::embedded_configs::<tool>_lua}`。
   `preload_builtin_renderers` 自动捎上它，apply 时会先看 user override
   `<config>/luban/configs/<tool>.lua` 再 fall back 到 embedded copy。
4. 图纸里用 `[config.<tool>]` 块（TOML）或 `configs.<tool> = {...}`（Lua）
   喂 cfg。Lua bps 可以 `register_renderer("<tool>", {...})` 覆盖 builtin
   （last-wins）。

不变量 9：`config_renderer.cpp` / `renderer_registry.cpp` 不引 `lua.h`；新 builtin
通过 `lua_frontend::wrap_embedded_module` 进 registry。core 一视同仁。

### Cut a release vX.Y.Z

```bat
:: bump version (one line, one file)
sed -i "s|VERSION [0-9.]*|VERSION X.Y.Z|" CMakeLists.txt
:: build + smoke locally (whichever toolchain you have on hand)
cmake --build --preset release
build\release\luban.exe --version

:: tag + push — CI does the build + release
git commit -am "Bump X.Y.Z" && git push
git tag -a vX.Y.Z -m "luban X.Y.Z — ..." && git push --tags
```

**Release**: single MSVC build per tag (DESIGN §1 Windows-first;
v1.0.5+ dropped the LLVM-MinGW flavor). `/MT` runtime → static-linked,
invariant 7 enforced.

| asset             | toolchain               | size    | notes                          |
| ----------------- | ----------------------- | ------- | ------------------------------ |
| `luban.exe`       | windows-latest cl /MT   | ~3 MB   | the binary                     |
| `luban-shim.exe`  | windows-latest cl /MT   | ~150 KB | shim twin for `.exe` PATH proxies |
| `SHA256SUMS`      | -                       | -       | sha256 for both binaries       |

CI verifies invariant 7: `dumpbin /DEPENDENTS` rejects vcruntime / msvcp /
api-ms-win-crt. Build job is `.github/workflows/build.yml` → single
`build` job that uploads release artifacts directly when triggered by
a `v*.*.*` tag push.

## User-facing env vars

调用方常用，install.ps1 + luban C++ 都识别：

| env                          | 默认                     | 说明                                                                                                                                                            |
| ---------------------------- | ---------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `LUBAN_INSTALL_DIR`          | `~/.local/bin`         | install.ps1 安装目标目录                                                                                                                                            |
| `LUBAN_FORCE_REINSTALL`      | unset                  | =1 时 install.ps1 跳过 SHA-命中短路                                                                                                                                  |
| `LUBAN_GITHUB_MIRROR_PREFIX` | unset                  | 反代 prefix（如 `https://ghfast.top`），重写 `github.com` / `*.githubusercontent.com` URL；**不**重写 `api.github.com`（公共 mirror 都 403 它）。注意 ghfast.top 限速严格，VN 网络下直连可能更快 |
| `LUBAN_EXTRACT_THREADS`      | min(8, hw_concurrency) | archive::extract worker 数（0 = 单线程）。llvm-mingw bundle 测 4 路对单流 ~5× 加速，超过 ~8 SSD 写饱和无意义                                                                          |
| `LUBAN_PROGRESS`             | unset                  | =1 强制开 progress bar（非 TTY 也开）                                                                                                                                 |
| `LUBAN_NO_PROGRESS`          | unset                  | =1 关 progress bar                                                                                                                                             |

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
- **GitHub mirror 限速**（ghfast.top / gh-proxy.com 等公共反代）：免费 IP
  限速到几 KB/s 是常态。VN/CN 网络如果直连 github.com 可达就**别**走 mirror——
  实测直连可比 mirror 快 25×。mirror 的真正用武之地是 GitHub 完全封堵
  的网络。

## Where to read more

- **Design**: `docs/DESIGN.md` — 唯一设计文档 (§1–§11)；MVP 概念层。
- **History**: `git log --oneline` — 每步独立 commit。

## Critical reusable functions (don't reinvent)

| Function                                                                       | File                              | Use                                                                                                                                               |
| ------------------------------------------------------------------------------ | --------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------- |
| `paths::{data,config,cache,state}_dir()`                                       | `src/paths.cpp`                   | XDG-resolved 4 homes                                                                                                                              |
| `paths::xdg_bin_home()`                                                        | `src/paths.cpp`                   | `~/.local/bin/`（不变量 5）                                                                                                                            |
| `proc::run(cmd, cwd, env_overrides)`                                           | `src/proc.cpp`                    | spawn with env merge                                                                                                                              |
| `download::download(url, dest, opts)`                                          | `src/download.cpp`                | HTTPS GET + SHA（Win32 走 curl_subprocess.cpp，POSIX 走 libcurl）                                                                                      |
| `curl_subprocess::download_to_file / fetch_text / head_content_length`         | `src/curl_subprocess.cpp`         | Win32 only — drives curl.exe via CreateProcessW，polling thread feeds `progress::Bar`                                                              |
| `progress::Bar`                                                                | `src/progress.{hpp,cpp}`          | 统一进度 UI：`↓ fetch / ↻ extract / ✓` glyph + Bytes/Items unit + TTY auto-detect + LUBAN_PROGRESS env                                                 |
| `archive::extract(zip, dest, on_progress)`                                     | `src/archive.cpp`                 | ZIP w/ traversal guard + 可选 progress cb                                                                                                           |
| `hash::verify_file(path, spec)`                                                | `src/hash.cpp`                    | SHA256 校验                                                                                                                                         |
| `env_snapshot::apply_to(env)`                                                  | `src/env_snapshot.cpp`            | PATH + injected_env overlay (xdg_bin_home + MSVC dirs + VCPKG cache)                                                                              |
| `applied_db::{is_applied,mark_applied,record_owned_shim,list_owned_shims,clear}` | `src/applied_db.{hpp,cpp}`        | flat-file state under `<state>/luban/`：bp 已应用集 + luban-owned shim 路径簿                                                                                |
| `iso_time::now()`                                                              | `src/iso_time.{hpp,cpp}`          | ISO-8601 UTC second-precision timestamp                                                                                                           |
| `luban_cmake_gen::regenerate_in_project(dir, targets)`                         | `src/luban_cmake_gen.cpp`         | 重渲项目 `luban.cmake`                                                                                                                                |
| `vcpkg_manifest::{add,remove,save}`                                            | `src/vcpkg_manifest.cpp`          | 安全编辑 `vcpkg.json`                                                                                                                                 |
| `lua_engine::Engine::eval_* / attach_registry / attach_resolver_registry`      | `src/lua_engine.cpp`              | Sandboxed Lua VM；可挂接两个 registry 让 `luban.register_*` 生效                                                                                           |
| `lua_frontend::wrap_renderer_module / wrap_resolver_fn / wrap_embedded_module` | `src/lua_frontend.cpp`            | **唯一**允许 include `lua.h` 的 .cpp（不变量 9）；把 lua refs / embedded module 包成 `RendererFns` / `ResolverFn` (std::function) 喂 core，shared_ptr<LuaRef> 管寿命 |
| `renderer_registry::RendererRegistry::{register_native, find_native}`          | `src/renderer_registry.{hpp,cpp}` | name → `RendererFns` (std::function pair)；per-apply 寿命；frontend 来源透明                                                                              |
| `resolver_registry::ResolverRegistry::{register_native, find_native}`          | `src/resolver_registry.{hpp,cpp}` | scheme → `ResolverFn` (std::function)；per-apply 寿命；frontend 来源透明                                                                                  |
| `config_renderer::render_with_registry`                                        | `src/config_renderer.cpp`         | config 块 → registry 单码 dispatch                                                                                                                   |
| `source_resolver::resolve / resolve_with_registry`                             | `src/source_resolver.cpp`         | source 块 → registry-first → C++ scheme dispatch (github)                                                                                          |
| `blueprint_lua::parse_file_in_engine`                                          | `src/blueprint_lua.cpp`           | 用调用方的 Engine parse Lua bp（`register_*` 副作用落到 Engine 挂的 registry）                                                                                  |
| `cli::Subcommand::forward_rest`                                                | `src/cli.hpp`                     | argv 透传（`luban run` 风格）                                                                                                                           |
| `applied_db::{is_applied, mark_applied, record_owned_shim, list_owned_shims, clear}` | `src/applied_db.{hpp,cpp}` | `<state>/luban/applied.txt` + `owned-shims.txt` 管理。apply 写入；`meta.requires` 门控 + `self uninstall` 读取                                            |
| `iso_time::now()`                                                              | `src/iso_time.{hpp,cpp}`          | ISO-8601 UTC 秒精度时间戳。lock `resolved_at` + bp source registry `added_at` / `commit` 用                                                              |
