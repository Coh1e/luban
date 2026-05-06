# luban — 设计

## 1. 一句话

**luban — 给一张图纸，搭一座 C++ 工坊。**

用户写一份**图纸（blueprint）**——Lua 或 TOML 配方；luban 照图纸把工具链、CLI 利器、dotfiles 一次拉齐摆好，PATH 接通，"在这台机器上写 C++"立即可用。

**工坊范围 = C++ 程序员的日常工作面**：
- **承重墙**：编译器 / cmake / vcpkg / ninja / git（必装）
- **工作台**：C++ 工作流相关 CLI 利器，ripgrep / fd / bat / fzf / delta / gh / jq / eza / zoxide（推荐）
- **挂墙的图**：相关工具的 dotfiles，git / bat / fastfetch / yazi / delta（按图纸渲染）

骨架是 C++，外延是服务 C++ 工作的 CLI 与配置。跨平台单二进制（~5 MB），零 UAC，XDG-first，与 uv/pipx/scoop 共享 `~/.local/bin/`。

## 2. 北极星

1. **Windows-first 但天然支持 POSIX 移植**（XDG-first 路径已预留）
2. **零 UAC、零系统级写入**——所有写在 `HKCU` + 用户家目录
3. **可逆**——`luban self uninstall` + 删 `include(luban.cmake)` 即完全退出
4. **AI 友好**——每个 verb 有 `--json` 结构化输出 + `AGENTS.md` 自动渲染

## 3. 受众价值

| 受众 | 价值 |
|---|---|
| 个人 / 学生 | 5 分钟搭起第一座工坊，含包管理；`luban new app` 直接出活 |
| 团队 onboarding | 给新人发**同一张图纸**——克隆仓库前先把工坊搭一致 |
| CI / 容器 | `LUBAN_PREFIX=/somewhere` 一份图纸搭出**临时工坊**，build 完丢弃 |
| AI agent / IDE | `luban describe --json` 把工坊状态导出；`AGENTS.md` 自动维护 |
| 跨机迁移 | 老工坊的图纸提交到 git，新机 `luban bp apply` 一份不少地复刻 |

## 4. 范围

| in scope（工坊功能）| out of scope（不属于工坊；明确拒绝）| 灰色（要做需先 ADR）|
|---|---|---|
| **承重墙**：C++ 工具链（llvm-mingw / cmake / ninja / mingit / vcpkg + opt. emscripten）| 通用应用安装（Office / 浏览器 / 媒体软件——winget 的事）| MSVC 工具链分发（候选；目前只 LLVM-MinGW）|
| **工作台**：C++ 工作流相关 CLI 利器（cli-quality 内置 + 用户图纸扩展）| 纯 shell 美化（zsh / tmux / oh-my-X——home-manager 的事）| per-project toolchain pin（rustup 风格）|
| **挂墙的图**：相关工具 dotfiles（`[config.X]` 内置 5 个 + `[file."path"]` 兜底）| 跳图纸的单工具安装（`luban tool install <pkg>` 反范式——一切走图纸）| 项目级图纸（`<project>/.luban/blueprints/`）|
| 工程脚手架：`luban new app/lib`，原生 + WASM | 替换 cmake / 发明 IR / 发明新 manifest 格式 | 跨编译（`luban build --target=...` zig cc 风格）|
| 依赖编辑：`luban add/remove` 编辑 vcpkg.json + 自动重渲 luban.cmake | 多用户 / 系统级安装（HKCU + 用户家目录 only）| TUI / 交互 wizard |
| 构建透传：`luban build` 自动 preset + cmake/ninja（必要时 emcmake 包裹）| 多版本共存（C++ ODR；luban 同名工具 last-wins）| luban registry mirror（内置 luban-bucket + github）|
| 环境集成：`luban env --user` HKCU PATH + LUBAN_*/VCPKG_ROOT | 二进制中心仓库（vcpkg 自家 binary cache 已够）| sigstore / sumdb 风格 transparency log |
| Shim 体系：`~/.local/bin/<tool>.cmd` + `luban-shim.exe` + `.shim-table.json` | 官方 IDE 插件（生成 compile_commands.json 即可）| |
| 自管理：`self update/uninstall` / `doctor` / `--json` | luban cloud / 跨机同步 / telemetry / luban.exe 嵌 Rust crate / DLL | dual-flavor release（MSVC/MinGW，议题 R） |

## 5. 需求 MoSCoW

### Must

- **R1** 单二进制：`luban.exe` ~5 MB（含 Lua；QJS 推迟到 v1.x），static-linked，无 DLL/Python/MSI
- **R2** 零 UAC：所有写入都在 `%LOCALAPPDATA%` / `%APPDATA%` / `HKCU`
- **R3** XDG-first：`LUBAN_PREFIX` → `XDG_*_HOME` → 平台默认
- **R4** 一键工坊：`luban bp apply main/cpp-base` 装 LLVM-MinGW + cmake + ninja + mingit + vcpkg；`bp apply main/cpp-base-wasm` / `cpp-base-msvc` 叠层加 emscripten / MSVC
- **R5** 透明工程胶水：`luban new` / `add` / `build`，用户无需编辑 `CMakeLists.txt`
- **R6** 可逆：`luban self uninstall` + 删 `include(luban.cmake)` 即完全退出
- **R7** 项目无 luban 也能编：`luban.cmake` 是标准 cmake 模块、git-tracked
- **R8** 一致 SHA256 校验：所有下载链路（cache 命中 / 流式）同走 `hash::verify_file`
- **R9** Two-tier 依赖：toolchain 经 `bp apply main/cpp-base`（工坊级，generation 持），项目库经 `luban add`（写 `vcpkg.json`，git-tracked）
- **R10** 29 入口（22 顶层 + bp 6 子命令 + 1 过渡 verb），5 组（blueprint / project / env / utility / transition）

### Should

- **R11** WASM 一等公民（`--target=wasm` + emscripten）
- **R12** 完整 shell completion（bash / zsh / fish / pwsh / clink）
- **R13** `describe --json` 稳定 schema=1
- **R14** POSIX 移植
- **R15** 工坊三层完整：承重墙（cpp-base）+ 工作台（cli-quality）+ 挂墙的图（dotfiles）
- **R16** 挂墙的图托管（[config.X] + Lua renderer + [file."path"] 兜底）

### Could

- **R17** Workspace（多 sibling 项目共享 vcpkg cache）
- **R18** Per-project toolchain pin（`--strict-pins` 在 CI 场景 hard-fail）
- **R19** TUI / 交互模式

### Won't

- 替换 cmake / 发明 IR / 发明 manifest 格式
- 写 Program Files / 动 HKLM / 需 admin
- 维护官方 IDE 插件
- luban cloud / telemetry
<!-- (议题 R) MSVC 构建 luban 自身已从非目标转为正式发布 flavor —— release 同时产出 MSVC (/MT) 与 MinGW 两个版本 -->
- luban.exe 嵌 Rust crate / DLL 等大型运行时依赖（Lua 是显式例外，详 §11）
- 多版本共存
- build-from-source（lib 依赖；vcpkg 已做这件事）
- **跳图纸的单工具安装**（`luban tool install <pkg>` 反范式——所有工具与配置走图纸；要装一个就写进图纸）
- **工坊外**：通用应用（Office/浏览器）/ 纯 shell 美化（zsh/tmux）—— winget / home-manager 各司其职

## 6. 8 不变量

1. **cmake 仍是主体**——不发明 IR、不替换 manifest
2. **`luban.cmake` schema 稳定**——已在野的项目 git-tracked。变更必须 v2-section + 兼容读回
3. **`vcpkg.json` 是项目依赖唯一真相**——不要在 `luban.toml` 加平行 `[deps]`
4. **`luban.toml` 可选**，承载项目级元数据：`[project] kind`（议题 AA） / `[scaffold]` / `[ports.X]` overlay/bridge port hint（议题 AB） / `warnings/sanitizer/preset/triplet` 偏好
5. **XDG-first 路径**，即使 Windows 上也是；bin shim 走 `~/.local/bin/`
6. **零 UAC、HKCU only**
7. **luban.exe 必须 static-linked**——新装 Win10 无 PATH 即可跑。两种实现都允许，CI 同时产出（议题 R, 2026-05-06）：
   - **MSVC** (`/MT`)：~3 MB，release 默认。`CMAKE_MSVC_RUNTIME_LIBRARY = MultiThreaded` 把 MS C runtime 静态嵌入；只剩 `kernel32 / user32 / shell32 / ole32 / advapi32 / bcrypt / winhttp` 这些 Win32 系统 DLL。
   - **MinGW** (`-static -static-libgcc -static-libstdc++`)：~6 MB，备选。同一 `cpp-base` 装的 llvm-mingw 编出来，dev/release bit-identical。
   - CI 用 `dumpbin /DEPENDENTS`（MSVC）/ `llvm-readobj --coff-imports`（MinGW）拒收任何含 `vcruntime / msvcp / api-ms-win-crt / libgcc_s / libstdc++ / libc++` 的依赖；invariant 7 在 release 流水线上被实测验证而不是文档约定。
8. **toolchain ≠ 项目库**——`luban add cmake` 必须被拒绝；toolchains 进 generation，libs 进 `vcpkg.json`

**放宽 1**：第三方 vendor 到 `third_party/<name>/` + LICENSE；**优先 single-header**，多文件需有充分理由——Lua 5.4 是当前唯一例外（脚本能力对 blueprint 不可替代；纯 C、可 sandbox、~200 KB 增量）。Python / V8 / QJS / DLLs 当前禁（QJS 是 v1.x 议题）。

> **Vendor**：把第三方源码直接拷进 `third_party/<name>/`，不依赖外部包管理 / 网络。R1（单二进制）+ R7（静态链接）的必然推论——fresh Win10 零网络依赖即可构建。代价：手动跟踪上游、自己负责 patch。

## 7. 三层架构

```
┌─ 用户层 ──────────────────────────────────────────────────────┐
│  luban CLI verbs：bp apply / new / build / add / ... (§16)    │
└───────────────────────────────────────────────────────────────┘
            │ 编辑 vcpkg.json / luban.toml；apply blueprint
            ▼
┌─ luban 中间层（"鲁班"）──────────────────────────────────────┐
│  luban.cmake               → 用户项目 git-tracked            │
│  ~/.local/bin/<tool>       → shim 出口，PATH 入口            │
│  <state>/generations/      → 当前激活快照                     │
│  <data>/store/             → 内容寻址 tool store             │
│  blueprints/<name>.lock    → 项目级解析锁                    │
└───────────────────────────────────────────────────────────────┘
            │ 通过 PATH + env_snapshot 调用
            ▼
┌─ 外部工具 ────────────────────────────────────────────────────┐
│  cmake / ninja / clang / vcpkg / git / ripgrep / fd / ...     │
│  （luban 下载并管理，但配置文件保持原样）                      │
└───────────────────────────────────────────────────────────────┘
```

**关键性质**：删 luban + 留下载工具 + 生成的 luban.cmake / 用户配置文件 → 仍是一套可工作环境。luban 是**可逆胶水**。

## 8. 五状态面（blueprint 流）

```
~/.local/share/luban/bp_sources/<source>/blueprints/<name>.{lua,toml}  ← source 图纸（主路；§9.10）
~/.config/luban/blueprints/<name>.{lua,toml}        ← 用户图纸（意图；user-local 兼容）
                ↓ luban bp apply
~/.config/luban/blueprints/<name>.lock              ← 解析快照（精确版本/sha256；含 source_commit）
                ↓
~/.local/share/luban/store/<artifact-id>/           ← 内容寻址 store（不可变）
                ↓ shim / hardlink / symlink
~/.local/bin/<tool>                                 ← PATH 入口
~/.config/<tool>/...                                ← 配置文件（XDG 标准位置）
                ↓
~/.local/state/luban/generations/N.json             ← 当前激活快照（atomic switch）
```

**部署原则（不变量级）**：luban 不接管已安装工具的配置目录。fastfetch 还是去 `~/.config/fastfetch/config.jsonc` 找配置——对 luban 无感。luban 只拥有：

| 目录 | 内容 |
|---|---|
| `~/.config/luban/blueprints/` | 用户写的图纸（git-trackable）|
| `~/.local/share/luban/store/` | 内容寻址 tool store |
| `~/.local/share/luban/files/` | 内容寻址 file store（symlink 用）|
| `~/.local/state/luban/` | generation / shim-table / external |

## 9. 图纸（blueprint）

### 9.0 概念三件：blueprint / tool / config

luban 的图纸描述"赋予一台机器某个能力"，自上而下三层：

- **blueprint（图纸 / 能力包）**——一组装在一起就能让机器获得某个能力的声明集合。
  例：`main/git-base` 让机器能用 git+LFS+GCM 工作；`main/cpp-base` 让机器能编 C++；
  `main/cli-quality` 让机器有现代化 CLI 工作台。**一张图纸 ≈ 一种能力**。
- **tool（工具 / 可执行物）**——实现能力靠的程序。一条 `tool` 声明对应
  PATH 上能调用的一个二进制（cmake / ninja / git / ssh / ...），由 luban 装在
  `~/.local/share/luban/store/<artifact-id>/` 并 shim 到 `~/.local/bin/`。
- **config（配置 / 设定）**——让 tool 按你的方式跑的 dotfile。一条 `config` 声明
  喂给一个 renderer（`templates/programs/X.lua`），渲出 `(target_path, content)`，
  drop-in 落到 XDG 路径（如 `~/.gitconfig.d/<bp>.gitconfig`）。

**关系**：

- blueprint 是顶层容器；tool 和 config 是它的内容；同一张图纸可只装 tool、只写
  config、或两者都有
- 同名的 `[tool.X]` 与 `[config.X]` 默认绑定同一个 X（如 `[tool.bat]` + `[config.bat]`）；
  显式绑定通过 `[config.X] for_tool = "Y"` 覆盖（**schema 化**关系），缺省 = X
- 两边各有独立 lifecycle：cli-quality 可以只写 `[config.git]`（不装 git，由
  git-base 装），或只写 `[tool.gh]`（不配 gh）。这是 Windows 现实——很多工具
  来自 Scoop / 系统 / 同事预配，luban 不必"也得装"才能配

**命名约定**：TOML 键全部**单数**（`[tool.X]` / `[config.X]` / `[file."path"]`），
跟 TOML 习惯（`[server.alpha]`、`[database.production]`）一致。C++ 内部
collection 字段保持复数（`std::vector<ToolSpec> tools`）；JSON 序列化（lock /
generation）保持复数 key（`"tools": {...}`）——各语言遵各自惯例。

**实施状态（2026-05-06）**：概念命名已对齐；TOML schema 键 + 渲染子系统命名
（`program_renderer` → `config_renderer`、`templates/programs/` → `templates/configs/`）
的代码侧 rename 是单独 PR。本节示例代码用目标命名（`[tool.X]` / `[config.X]`），
当前 .toml 文件实际写的还是历史的 `[tools.X]` / `[programs.X]` 复数键——见
§24.1 决议 **P**。

### 9.1 Lua 形态（first-class，DSL 风格）

```lua
-- ~/.config/luban/blueprints/dev.lua
local is_win = luban.platform.os() == "windows"

return {
  name = "dev",
  description = "my daily dev environment",

  tool = {
    ripgrep = { source = "github:BurntSushi/ripgrep" },
    fd = { source = "github:sharkdp/fd" },
    bat = { source = "github:sharkdp/bat" },
  },

  config = {
    git = {
      userName = "Coh1e",
      userEmail = luban.env.get("GIT_EMAIL") or "fallback@x.com",
      aliases = { co = "checkout", br = "branch" },
      core = { editor = is_win and "vim" or "nvim" },
    },
    bat = { theme = "ansi", style = { "numbers", "changes", "header" } },
  },

  meta = { requires = { "main/cpp-base" } },
}
```

home-manager 的 home.nix 但用 Lua —— 表语法读起来 declarative，全脚本能力托底（`luban.platform.*` / `luban.env.*` / 条件 / 循环 / require 等）。

### 9.2 TOML 形态（静态意图最简形）

```toml
schema = 1
name = "cli-quality"

[tool.ripgrep]
source = "github:BurntSushi/ripgrep"

[config.git]
userName = "Coh1e"
[config.git.aliases]
co = "checkout"

[file."~/.config/edge-tool/config"]
content = """
key=value
"""
mode = "replace"
```

### 9.3 JavaScript 形态——不在 v1.0 范围

议题 T：QJS / `.js` 图纸**v1.x 候选，本期不做**。详 §24.1。

### 9.4 Lock 文件

`<name>.lock` JSON，与图纸同目录、同名：

```json
{
  "schema": 1,
  "blueprint_name": "dev",
  "blueprint_sha256": "<sha of source>",
  "resolved_at": "2026-05-05T...",
  "tools": {
    "ripgrep": {
      "version": "14.0.3", "source": "github:BurntSushi/ripgrep",
      "platforms": {
        "windows-x64": { "url": "...", "sha256": "...", "bin": "rg.exe", "artifact_id": "ripgrep-14.0.3-windows-x64-<hash8>" }
      }
    }
  },
  "files": { "~/.config/bat/config": { "content_sha256": "...", "mode": "replace" } }
}
```

`luban bp apply` 第一次解析时写 lock；后续 apply 只 replay；`bp apply --update <name>` 才重写。**lock 应当提交 git** —— 可重现性锚点。

### 9.5 Source resolver（双模式）

- **自动**：`source = "github:owner/repo"` → 拉 GitHub releases/latest 解析平台资产 + sha256 → 写 lock
- **手写**：直接在图纸 inline `[[tool.X.platform]] url=... sha256=... bin=...`，绕过解析

cpp-base 自身用**内置 bucket**：`bucket/<tool>.toml` 是 luban 仓里的静态 manifest；cpp-base 引用按名查表。

### 9.6 Env / Capture 段

图纸声明环境变量 + 外部环境采集——决定哪些 env 写 HKCU、哪些只在 luban 派生子进程里出现：

```toml
# cpp-base.toml — LLVM 工具链层
[env.persistent]
# 写 HKCU；任意 fresh shell（IDE / VSCode / 普通 cmd）都能看到
VCPKG_ROOT = "{{ tools.vcpkg.root }}"
VCPKG_DOWNLOADS = "{{ cache }}/vcpkg/downloads"
VCPKG_DEFAULT_BINARY_CACHE = "{{ cache }}/vcpkg/binary"
```

```toml
# cpp-base-wasm.toml
[env.persistent]
EM_CONFIG = "{{ config }}/emscripten/config"
```

```toml
# cpp-base-msvc.toml — MSVC 那 ~30 个 sb env 全在这里
[env.injected]
# 仅 luban-spawned 子进程可见；不写 HKCU
# 由下方 capture provider 填入，不用手列

[capture.msvc]
provider = "msvc-vcvarsall"   # luban 内置 provider
mode = "injected"              # 采集到的 env 全部进 injected_env，不污染 HKCU
```

模板引用（apply 时一次性求值，结果固化进 lock 与 generation）：
- `{{ tools.<name>.root }}` / `{{ tools.<name>.bin }}` — store 里的工具路径
- `{{ home }}` / `{{ config }}` / `{{ cache }}` / `{{ state }}` / `{{ data }}` — home + XDG 四个 home

为什么 `{{ }}` 而非 `${...}` 或单 `$x`：避免与 shell 变量展开混淆——配置文件里看到 `${X}` 容易误读为 shell；`{{ X }}` 一眼就是模板，没误导。语法**受限**（只支持点路径，不支持 `if` / `for` / 字符串拼接）；想要表达式请写 Lua 图纸。

**capture provider 是白名单**——luban 内置一份小列表（`msvc-vcvarsall` 等）；图纸里**不能**写任意 capture 命令（安全）。

**PATH 不进 `[env]`**——见 §18。

### 9.7 Shape 由 TOML 定形；Lua DSL 主用输入

`BlueprintSpec` 的**字段与层级以 TOML 形态定形**——TOML 的 section / sub-table 写法把层级显形（`[tool.X.platform]` 一目了然），是天然的"shape 契约"。Lua **不是另一套 schema**，是同一 shape 的另一种输入形态。

```
TOML shape 契约  ←─映射─→  Lua 输入  ──fold─→  BlueprintSpec (C++ struct)
```

| 壳 | 角色 | 用法 |
|---|---|---|
| TOML | **shape 契约 + 极静态用户图纸入口** | 每加一个字段先在 TOML 定形（层级 + 类型 + 约束）；用户极简图纸（声明几工具几 dotfile，零计算）直接写 TOML |
| **Lua DSL** | **主推输入形态**——declarative 表语法 + 完整表达力（条件 / 平台分支 / env / require）| **内置图纸主用**（`main/cpp-base*` / `main/cli-quality`）；用户图纸推荐起手 |

**Lua 输出必须塌缩为 TOML shape**——多余字段忽略或报错；缺字段按默认值或验证报错。

为什么 TOML 主管 shape：
- TOML 的 section 边界比 Lua 的嵌套对象**更难误读**
- 加新字段流程：**先动 TOML 文档**（写清字段路径、类型、约束）→ 再动 `BlueprintSpec` → 再让 Lua 解析层把它接住——文档先行强制把 schema 决策摆到台面

为什么 Lua DSL 主推输入：
- 内置图纸大都需要平台分支 / 路径合成（cpp-base 选 asset、cpp-base-wasm 查 cpp-base 路径、cpp-base-msvc 在 Linux 上 no-op）
- 没有计算时，Lua 表语法读起来与 TOML 同样 declarative
- home-manager（home.nix）/ wezterm / neovim 多年验证过

```lua
-- Lua DSL：内置图纸主用
local target = luban.platform.target()  -- "windows-x64" / "linux-x64" / "macos-arm64"
return {
  name = "cpp-base",
  tool = {
    cmake = { source = "github:Kitware/CMake", version = "3.30.0" },
    ["llvm-mingw"] = target == "windows-x64"
      and { source = "github:mstorsjo/llvm-mingw" }
      or nil,           -- 非 Windows no-op
  },
  env = {
    persistent = {
      VCPKG_ROOT = luban.tool_root("vcpkg"),
    },
  },
}
```

```toml
# 等价 TOML shape；用模板 substitution 替代表达式
schema = 1
name = "cpp-base"

[tool.cmake]
source = "github:Kitware/CMake"
version = "3.30.0"

[env.persistent]
VCPKG_ROOT = "{{ tools.vcpkg.root }}"
```

**没必要发明新 luban DSL**——Lua 已经是好 DSL；自造只增学习成本不增价值。

### 9.8 内置 blueprint 命名空间（已撤场）

v0.x 用过 `embedded:<X>` 形式的保留前缀来引用编进二进制的 3 张内置图纸（cli-quality / cpp-base / git-base）。议题 AG（2026-05-06）终态决议把这套机制整体撤场——luban.exe 不再嵌入任何 bp，所有 bp 来自外部注册的 bp source（详 §9.10）。

历史细节查 git history；该前缀在 v1.0 是硬错误，引用其旧示例语法已无意义。

### 9.9 Blueprint 携带逻辑：扩展点（resolver / renderer / post_install）

Blueprint 不止是数据——Lua 形态可以**同时注册扩展函数**，让"从哪下"和"配置文件怎么生成"这些 luban 在 apply 时执行的动作变成 blueprint 自带能力。这是 home-manager / Nix module 的核心 insight 移植到 luban。

#### 三个扩展点

| 扩展点 | 注册 API | 用途 | 触发时机 |
|---|---|---|---|
| **Source resolver** | `luban.register_resolver(scheme, fn)` | 把 `source = "<scheme>:..."` 解析成 url + sha256 + bin | apply → resolve |
| **Config renderer** | `luban.register_renderer(name, fn)` | 把 `[config.X].cfg` + ctx 渲染成 (target_path, content) | apply → render |
| **Post-install hook** | `[tool.X.post_install]` = 脚本相对路径 | extract 后跑一次性 setup（如 vcpkg bootstrap） | apply → fetch 后、shim 前 |

post_install **只接脚本路径**（必须在 extracted artifact 内），**不接 Lua 函数**——降低安全表面。想要 Lua 写"装后操作"请用 renderer。

#### Inline：spec 与扩展同在一个 .lua 文件

```lua
-- main/cpp-base-wasm.lua  （内置图纸示例）

-- (1) 注册 resolver：emscripten 在 Google Storage，不是 GitHub releases
luban.register_resolver("emsdk", function(spec)
  return {
    url = string.format(
      "https://storage.googleapis.com/webassembly/emscripten-releases-builds/win/%s/wasm-binaries.zip",
      spec.version),
    sha256 = "...",
    bin = "emscripten/emcc.bat",
  }
end)

-- (2) 注册 renderer：写 ~/.config/emscripten/config
luban.register_renderer("emscripten-cfg", function(cfg, ctx)
  local content = string.format(
    "LLVM_ROOT = '%s'\nNODE_JS = '%s'\nBINARYEN_ROOT = '%s'\nEMSCRIPTEN_ROOT = '%s'\n",
    ctx.tools["llvm-mingw"].root .. "/bin",
    ctx.tools.node.bin,
    ctx.tools.emscripten.root,
    ctx.tools.emscripten.root .. "/emscripten")
  return ctx.config .. "/emscripten/config", content
end)

-- (3) 返回 spec —— 用字符串名引用上面注册的扩展
return {
  name = "cpp-base-wasm",
  tool = {
    emscripten = {
      source = "emsdk",                  -- ↑ resolver 名
      version = "1724b50443d92e23ef2a56abf0dc501206839cef",
      depends = { "node" },
    },
  },
  config = {
    emscripten = {
      cfg = {},                           -- 渲染器从 ctx.tools 取路径，cfg 留空
      renderer = "emscripten-cfg",        -- ↑ renderer 名
    },
  },
  meta = { requires = { "main/cpp-base" } },
}
```

#### TOML 只能"引用"扩展，不能"注册"

```toml
[tool.ripgrep]
source = "github:BurntSushi/ripgrep"   # luban 内置 resolver

[config.git]
renderer = "git"                       # luban 内置 renderer
[config.git.cfg]
userName = "Coh1e"
```

TOML 图纸只能用 luban 内置或其他已应用 blueprint 注册过的扩展——它写不了 `register_*`。这是 TOML "极静态壳"的自然推论；想注册自己的扩展请改 Lua 形态。

#### luban 内置扩展走同一机制

`source_resolver_github` / `templates/configs/<X>.lua` 五个 renderer 在 luban 启动时通过同一套 `register_*` API 注入——**无双码路径**。luban 自己的扩展和图纸自带的扩展在同一注册表里，公平竞争。

#### 注册表语义与冲突

- **注册表 = 全局**：所有当前 generation 应用过的 blueprint 共享 resolver / renderer 注册表
- **同名覆盖 + warn**：后注册者覆盖前注册者（与 §12.1 叠层 last-applied wins 一致）
- **退层**（`bp unapply`）→ 该层注册的扩展从注册表移除，下次 apply 时其他层覆盖关系重排

#### Sandbox 边界

`register_*` 内部的函数运行在 blueprint 的 Lua VM 里，sandbox 同 §10.1：

| 允许 | 拒绝 |
|---|---|
| 读 ctx：`tools[X].root/bin` / `home/config/cache/state/data` / `platform` | `os.execute` / `io.popen` |
| 字符串处理、表操作、条件、循环、`require`（受限路径） | 任意 fs 读写 |
| `luban.platform.*` / `luban.env.get(name)` / `luban.download(url, sha256)` 受限下载 | 任意网络（绕过 luban.download） |
| 返回数据结构（resolver 返回 url+sha256+bin；renderer 返回 path+content） | 改环境变量 / 影响其他 blueprint 状态 |

#### 这一节解决的硬编码特例

§11 部署管线里两个 v0.x component.cpp 硬编码特例（vcpkg bootstrap、emscripten config）在这套机制下都被收编：

| 旧 hardcode | 新归属 |
|---|---|
| vcpkg post-extract `bootstrap-vcpkg.bat` | `main/cpp-base.lua` 里 `[tool.vcpkg.post_install] = "bootstrap-vcpkg.bat"` |
| emscripten 配置文件写入 | `main/cpp-base-wasm.lua` 里注册的 `emscripten-cfg` renderer |

luban 核心不再有"vcpkg / emscripten"这两个名字。

### 9.10 Bp Source 分发模型

> **状态**：v1.0 终态已落地（议题 AG，2026-05-06）。luban.exe **零内嵌 bp**——
> 所有 bp（含基础 3 件 git-base / cpp-base / cli-quality）都从外部
> 注册的 bp source 拉取。`embedded:<X>` 是硬错误，指向迁移路径。

#### 9.10.1 命名理由

复用 luban schema 已有的 `source` 一词。tool 层 `[tool.X] source = "github:..."` 表"这个工具从哪下"；bp 层 `luban bp source add main <url>` 表"这批 bp 从哪下"。平行概念、平行用词。心智模型对齐 scoop bucket / brew tap / vcpkg registry，但不引入新词。

#### 9.10.2 文件系统布局

- **注册表** `~/.config/luban/sources.toml`：

  ```toml
  [source.main]
  url = "https://github.com/<org>/luban-main"
  ref = "main"               # branch / tag / sha
  commit = "abc123..."       # 当前 sync 到的 commit

  [source.team]
  url = "https://github.com/<team>/luban-bps"
  ref = "v2024.05"
  commit = "def456..."
  ```

- **本地副本**：`~/.local/share/luban/sources/<name>/`（git clone）
- **source 仓库内部布局**：
  - `<repo>/blueprints/<name>.{toml,lua}` 主体
  - `<repo>/scripts/` 共享 post_install 脚本（可选）
  - `<repo>/source.toml` source 元数据（可选：维护者、最低 luban 版本约束）

#### 9.10.3 CLI 动词

5 个新 sub-verb，归入 `bp` group（不新增 group）。`bp src` 是 `bp source` 的短别名。

```
luban bp src add <url-or-shorthand> [--name <name>] [--ref <ref>] [--yes]
luban bp src rm <name>
luban bp src ls                       # 含每 source 当前 commit
luban bp src update [<name>]          # 重新 fetch 一个或全部
luban bp search [<pattern>]           # 跨已加 source 找 bp
```

`add` 接受 4 种输入形态，名字自动推导：

| 输入 | 等价 URL | 自动 name |
|---|---|---|
| `Coh1e/luban-bps` | `https://github.com/Coh1e/luban-bps` | `luban-bps` |
| `https://github.com/Coh1e/luban-bps` | (照抄) | `luban-bps` |
| `D:\Projects\luban-bps` | `file:///D:/Projects/luban-bps` | `luban-bps` |
| `file:///D:/Projects/luban-bps` | (照抄) | `luban-bps` |

`--name <name>` 覆盖默认；`add` 重名时直接覆盖（trust prompt 重跑），无需 rm 再 add。

verb 计数 29 → 34（4 source + 1 search）。

#### 9.10.4 解析优先级

`luban bp apply <X>` 的 X 解析：

- **显式限定** `<source>/<bp>`：总是确定来源（如 `main/cpp-base` / `team/onboarding`）
- **裸 `<bp>`**：搜索 user-local → 各 source（注册顺序）；多源命中时报错并要求显式限定
- **`embedded:<bp>`**：硬错误，提示走 `bp src add <url>` 注册 source
- **`<path>` / `file:<path>`**：本地文件，已支持

#### 9.10.5 Bootstrap 路径

fresh 机器只有 luban.exe，怎么搞到第一张 bp？luban 内置 HTTPS 下载（src/download.cpp）+ tarball extract（src/archive.cpp 走 miniz）。流程：

```pwsh
luban bp src add Coh1e/luban-bps --name main   # 一次性 trust prompt
luban bp apply main/git-base                   # 装 git
luban bp apply main/cpp-base                   # C++ 工具链
```

`bp src add` 走 GitHub codeload zip（不需要本机有 git）；`apply main/git-base` 完成后机器才有 git。**没有离线 fallback**——fresh box 必须能联网拉一次 bp source；之后所有工具的下载继续走 luban 自带的 HTTPS。

实现上是否给 luban 烤一张最小 bootstrap snapshot 作离线 fallback：**不烤**（议题 AG 终态决议）。一旦烤，"全 source 化"就是假的；离线场景靠 file:// + 本地 clone 解决。

#### 9.10.6 Lock file 升级

```jsonc
{
  "name": "cpp-base",
  "source": "main",                       // 新字段；user-local 时为 null
  "source_commit": "abc123...",           // 新字段，source HEAD 时刻
  "content_sha": "...",                   // 已有，bp 文件内容 hash
  "tools": [...]                          // 已有
}
```

#### 9.10.7 信任边界

- bp 含 Lua 扩展（resolver / renderer / post_install）= 信 source 维护者代码
- `bp source add` 时 luban 打 ⚠ + 显示 url + 首次 commit + 让用户 `[y/N]` 确认
- 未来加签名验证（GPG / sigstore）—— v1.x 工作

#### 9.10.8 保留名

source 名 `embedded` / `local` 受保护——分别保留给"内嵌 bootstrap snapshot 来源"和"user-local 文件目录"的概念性命名空间。`main` 是默认官方 source 名（用户可重定向 URL，但不能新建同名 source）。

#### 9.10.9 与议题 P / F 的关系

- **P（schema 单数化）**：source 仓里的 .toml/.lua 用 §P 后的单数 schema（`[tool.X]` / `[config.X]`），source 模型上线时 §P 重命名应已完成
- **F（embedded 命名空间）**：F 描述的 v0.x 内嵌前缀已由 AG 整体撤场，无 alias 期；`embedded:<X>` 在 v1.0 直接硬错误

## 10. 嵌入脚本：Lua 5.4

### 10.1 Lua 5.4（一等）

- vendored 到 `third_party/lua54/`（60 .c/.h 文件，excl. lua.c/luac.c CLI 入口；MIT）
- `src/lua_engine.{hpp,cpp}` RAII Engine 类
- 启动时 sandbox：strip `io.{open,popen,read,write,...}` / `os.{execute,exit,remove,rename,tmpname,setlocale}` / `package.{loadlib,cpath,searchpath}` / `loadfile` / `dofile` / `debug` / `print`
- 注入 `luban.*` API：`luban.version` / `luban.platform.{os,arch,target}()` / `luban.env.get(name)` / `luban.tool_root(name)` / `luban.download(url, sha256)` / `luban.register_resolver(scheme, fn)` / `luban.register_renderer(name, fn)`
- 用途：用户 Lua 图纸 + per-tool program renderer（内置 5 个：git/bat/fastfetch/yazi/delta）+ blueprint inline 注册的 resolver / renderer（§9.9）

### 10.2 为什么 Lua（不是 Python / QJS）

- Lua 表语法是 DSL 写法的 sweet spot（home-manager / wezterm / neovim 验证过）——读起来 declarative
- Lua ~200 KB 极致紧凑；CPython 嵌入 ~10 MB+ 破 R1 + 静态嵌不可行（动态依赖 libpython 又破 R7）；QJS ~1.5 MB 是 v1.x 议题（议题 T）
- 内置 renderer 全 Lua → 一份代码，零双份维护成本

## 11. 工具部署管线

```
luban bp apply X
   ├─ 读 X.{lua,toml} → BlueprintSpec
   ├─ 读 X.lock（若有）；否则 source_resolver 解析 + 写 lock
   ├─ tool 拓扑 sort（depends 字段）
   ├─ for each tool:
   │     external_skip 探测（which <tool>，已装且版本满足 → 记 external，跳过）
   │     download → verify sha256 → extract → <data>/store/<artifact-id>/
   │     post_install 若有（如 vcpkg 的 bootstrap-vcpkg.bat）
   │     shim → ~/.local/bin/<tool>（cmd shim on Win，symlink on POSIX）
   ├─ for each [config.X]:
   │     按 renderer 名查 extension_registry（user override `<config>/luban/configs/<X>.lua` 优先；
   │       回退 luban 内置 `templates/configs/<X>.lua`）
   │     调 M.render(cfg, ctx) → 文本
   │     写到 M.target_path(cfg, ctx)（工具的标准 XDG 路径）
   ├─ for each [files]:
   │     replace mode → 备份原文件到 <state>/backups/N/，写 inline content
   │     drop-in mode → 直接写到 drop-in 子目录（不动顶层文件）
   ├─ for each [env.persistent / injected]:
   │     替换 {{}} 模板 → 进 generation 的 persistent_env / injected_env 表
   ├─ for [capture.X]:
   │     跑 luban 内置 provider → 产物 env 进 mode 对应的 env 表
   └─ 写新 generation（<state>/generations/N+1.json + 切 current symlink）
```

### 11.1 Artifact

> **Artifact** = store 里**已下载/校验/解压、内容寻址、不可变**的工具二进制。多个 blueprint 引用同一 artifact 不重复下载；rollback 是切指针不动磁盘内容；gc 安全（无 generation 引用即可删）。从用户视角它就是 Tool 的"装好后的样子"，名词上不与 Tool 分开。

`<artifact-id>` = `<name>-<version>-<platform>[-<abi>]-<hash8>`，hash8 = first8hex(sha256(canonical_input_json))。同一工具不同版本/平台/abi 可并存于 store；用户 generation 决定哪个进 PATH。

### 11.2 External skip

`luban bp apply` 前 `which <tool>` 探测；已存在版本满足 blueprint 约束 → 记 `external` 不重装。解 scoop 等已装用户与 luban 装名重复的 PATH 冲突（详 §23 决策）。

### 11.3 Per-tool config renderer

每工具一个 `templates/configs/<tool>.lua` 模块（编进二进制），导出 `M.render(cfg, ctx) → string` + `M.target_path(cfg, ctx) → path`。详例见 §9.9 inline 注册形态——**机制与 §9.9 的 `register_renderer` 完全一致**：内置走启动时注册，blueprint 自带 renderer 走 apply 时注册，同一注册表，无双码路径。

`ctx` 由 luban 提供：`{ home, config, cache, state, data, blueprint_name, platform, tools{name → {root, bin}}, ... }`。用户可在 `<config>/luban/configs/<name>.lua` 写覆盖内置。

### 11.4 文件部署模式

| mode | 行为 |
|---|---|
| `drop-in`（config renderer 默认）| 写到独立子目录（如 `~/.gitconfig.d/`）；用户在顶层加 `[include]`；luban 永不动顶层 |
| `replace`（[files] 整文件管理）| apply 前自动备份原文件到 `<state>/backups/N/`，rollback 时还原 |

## 12. Generation & rollback（nix profile 风格）

**Generation = 当前叠层栈的快照**——记录此刻摞着哪些 Blueprint，每层贡献了哪些 Tool / Config / File / EnvVar。多张图纸像 layer 一样叠（cpp-base 是承重墙，cpp-base-wasm / cpp-base-msvc 是可选层，cli-quality 是工作台层，用户自图纸是装修层），Generation 是"叠完之后的样子"。

```
<state>/
├── generations/
│   ├── 1.json
│   ├── 2.json
│   └── current  → 7.json   (symlink)
├── backups/
│   ├── 5/<original-files>...
│   └── 6/<...>...
└── ...
```

每次 `bp apply` / `bp unapply` 改变栈 → 写新 generation 文件 + 改 `current` symlink。**atomic switch** = 改 symlink 一步。

`luban bp rollback [N]`：把 `current` 改向 N（默认上一代）→ 重写 shim + 重部署 files + 重写 HKCU env diff。同样 atomic。

### 12.1 叠层冲突解决

两层装同一个 Tool（不同版本）/ 写同一个 File / 同一个 EnvVar：**last-applied wins + warn**——后 apply 的层覆盖前层（同 nix profile / home-manager 习惯）。

Configs 走**每层独立 drop-in**：`~/.gitconfig.d/cli-quality.gitconfig` + `~/.gitconfig.d/dev.gitconfig`，用户顶层 `~/.gitconfig` 用 `[include]` 拉整目录；退某层 = 删那一份 drop-in，干净可逆。

## 13. 模块边界

### 13.1 顶层

| 模块 | 文件 | 对外 API（关键） | 谁调用 |
|---|---|---|---|
| `cli` | `src/cli.{hpp,cpp}` | `Subcommand` / `ParsedArgs` / `forward_rest` | `main.cpp` 注册所有 verb |
| `paths` | `src/paths.cpp` | `data_dir()` / `cache_dir()` / `state_dir()` / `config_dir()` / `toolchain_dir(rel)` | 几乎所有模块 |
| `proc` | `src/proc.cpp` | `proc::run(cmd, cwd, env_overrides)` | spawn 子进程的 verb |
| `download` | `src/download.cpp` | `download(url, dest, opts)` 流式 SHA256；POSIX libcurl / Win WinHTTP | `component`、未来 `store` |
| `archive` | `src/archive.cpp` | `extract(zip, dest)` zip-slip 防护（miniz） | `component` |
| `hash` | `src/hash.cpp` | `verify_file` / `HashSpec parse`；POSIX OpenSSL EVP / Win bcrypt | `download` / `component` |
| `env_snapshot` | `src/env_snapshot.cpp` | `apply_to(env)` 注 PATH + VCPKG_* + EM_CONFIG (+ MSVC env) | `proc::run` 的调用者 |
| `perception` | `src/perception.cpp` | `host::info() → JSON`：OS / CPU / RAM / SIMD / tools / XDG | `describe --host` / AGENTS.md 渲染 |
| `msvc_env` | `src/msvc_env.cpp` | vswhere 探测 + vcvarsall capture（v1.0 由 `main/cpp-base-msvc` 的 `[capture.msvc]` provider 调用，议题 M）| `extension_registry` (provider) |

### 13.2 工坊系统层（shim + HKCU 集成）

v1.0 单轨化后，老的 v0.x 模块（`manifest_source` / `scoop_manifest` / `component` / `registry` / `commands/shim_cmd`）退役——它们的功能被 `blueprint_apply` 取代。剩下 plumbing：

| 模块 | 文件 | 职责 |
|---|---|---|
| `shim` | `src/shim.cpp` | `.cmd` shim 写出 + `.shim-table.json` 碰撞检测；输出走 `~/.local/bin/`（不变量 5）|
| `shim_exe` | `src/shim_exe/main.cpp` | 独立二进制 `luban-shim.exe`；硬链接成 `<alias>.exe` |
| `win_path` | `src/win_path.cpp` | HKCU PATH / 环境变量读写、`WM_SETTINGCHANGE` 广播 |

`shim` 不再被独立 verb 暴露——`bp apply` / `bp rollback` / `bp unapply` 内部自动维护 `~/.local/bin/`（议题 H：`shim` verb 退为 plumbing）。

### 13.3 项目 / 工程层

| 模块 | 文件 | 职责 |
|---|---|---|
| `vcpkg_manifest` | `src/vcpkg_manifest.cpp` | 安全编辑 `vcpkg.json`（add/remove/save，原子写） |
| `lib_targets` | `src/lib_targets.cpp` | 表驱动：`port → find_package + targets[]`（覆盖常见 upstream port；用户私有 port 走 `luban.toml [ports.X]`，议题 AB）|
| `luban_cmake_gen` | `src/luban_cmake_gen.cpp` | 渲染并原子写 `luban.cmake`（v1 schema） |
| `luban_toml` | `src/luban_toml.cpp` | 解析 `luban.toml` 偏好；`cpp` ∈ {17,20,23} 校验 |
| `marker_block` | `src/marker_block.cpp` | AGENTS.md 区域注入引擎（regex） |
| `commands/{add,target,which_search,...}` | 各自 | 详 §16 verb 表 |

### 13.4 Blueprint 引擎模块

| 模块 | 职责 |
|---|---|
| `lua_engine` | Lua 5.4 VM + sandbox + luban API |
| `extension_registry` | 全局 resolver / renderer 注册表（§9.9）；luban 内置与 blueprint 注册同走此表；同名后注册者覆盖 + warn |
| `blueprint_toml` | TOML 图纸 → BlueprintSpec |
| `blueprint_lua` | Lua 图纸 → BlueprintSpec |
| `blueprint_lock` | JSON lock 读写 |
| `source_resolver` + `source_resolver_github` | inline pass-through + `github:owner/repo` 真实 GitHub releases API |
| `store` + `store_fetch` | `<data>/store/<artifact-id>/` 物化（fetch 与 identity 拆 TU 让 tests 不需网络栈）|
| `external_skip` | `which <tool>` 探测；记 `<state>/external.json` |
| `file_deploy` | replace / drop-in 部署 + 备份 |
| `config_renderer` + `lua_json` | `[config.X]` 调度到 extension_registry 命名的 Renderer |
| `platform` | host_os / arch / triplet helper |
| `generation` | snapshot + atomic switch + rollback |
| `blueprint_apply` | parse → resolve → fetch → render → deploy → snapshot 编排 |
| `commands/blueprint` | apply / unapply / ls / status / rollback / gc 子命令调度（详 §16）|
| `commands/migrate` | 旧 installed.json → generation 1 |

## 14. 领域实体

### 14.1 系统侧（Alias / Shim）

> v0.x 的 `Component` noun 在 v1.0 被 `Tool`（§14.3）替代——同一概念（装好的工具）；Component 不再是 first-class noun。

**Alias**（可执行别名）：`{ alias, exe, prefix_args[] }`——一个 Tool 可以暴露 1+ Alias（如 mingit 暴露 git/curl/...）；shim 写出时按 alias 一对一生成 `~/.local/bin/` 入口。

**Shim**（Alias 物化到 PATH）：`<alias>.cmd` text shim + `<alias>.exe`（luban-shim.exe 硬链接）+ `.shim-table.json` 反查表（`bp apply` 全量维护，议题 H）。

### 14.2 项目侧

**Project**：`{ root, kind, vcpkg_json, luban_toml?, luban_cmake, presets, targets[] }`

**Project kind**（三种；决定初始 scaffold + 行为分支；显形写在 `luban.toml [project] kind`）：

| kind | 主 Target | 分发模型 | scaffold 关注 |
|---|---|---|---|
| **app** | 1 个 exe | "给人跑"——zip 个 exe / 上传 release | CMakeLists 简单；不需要 install + export |
| **lib** | 1 个 library | "给别的项目 link"——Port → Registry → vcpkg | CMakeLists 默认带 install + export 套路；可打成 Port |
| **wasm-app** | 1 个 exe 编到 wasm | "给浏览器跑"——index.html + .wasm | emcmake 包裹；emscripten 工具链 |

`kind` 显形让 `luban port new --from-project` / doctor / build 都能照 kind 分支（如 doctor 警告 app 项目写了 `install(EXPORT)`）。

**Target** 是 **cmake 的概念**，不是 luban 发明——`add_executable(foo)` / `add_library(bar)` 在 cmake 里产生 target。luban 名词里的 `Target` 是个薄记录（`{name, kind: exe/lib, subdir}`），**target 真正活在 CMakeLists.txt 里**。luban 只通过两个手术式动作触碰：
- `target add foo [--lib]` —— 在 CMakeLists.txt 写一段 `add_executable/library` + 同步 luban.cmake
- `target rm foo` —— 反向
其他时候 luban 只读（`describe` 列出，build 让 cmake 自己处理）。

一个 Project 可以有**多个 Target**——`luban new app foo` 默认 1 个 exe；后面 `target add foo-core --lib` 加内部 lib，`target add foo-tests` 加测试 exe。**主 Target 决定 Project kind**（约定，非硬规则）。

**Output**（实物）：`luban build` 生成的二进制——`build/<preset>/<target>.exe` / `lib<target>.a` / `.dll` / `.pdb` / `compile_commands.json` 等。Target 是声明，Output 是实物；luban 不管 Output 生命周期（cmake build dir 自管），但 `describe --json` 列出来；`luban run <target>` 跑的是该 Target 对应的 Output。

**Dependency**：`{ port, version_ge?, find_package, targets[] }`；`lib_targets.cpp` 内置常见 upstream port 的表查映射；用户自写的 overlay/bridge port 通过 `luban.toml [ports.X]` 提供 hint（议题 W 落地需要）。

**Port**（统一名词，三类）：

| 类 | 来源 | 机制 |
|---|---|---|
| **Upstream port** | microsoft/vcpkg ports tree | vcpkg 默认；用户 `luban add <pkg>` 命中此类 |
| **Overlay port** | 用户自写的 port（本地或私有 git）| vcpkg overlay-ports；`vcpkg-configuration.json` 声明 |
| **Bridge port** | 非 vcpkg 来源（Rust crate / 其他生态）| luban 写一份 wrap 转成 vcpkg port 形态（v1.x 议题：bridge port 脚手架）|

三类对 vcpkg / cmake 都是 port 形态——下游消费方（`find_package` + cmake target）无差别。Bridge port 的 v1.0 状态：架构留位、不实现脚手架；用户想做手动写 portfile.cmake 即可。

**Registry**（vcpkg 概念，luban 借用）：装多个 Port 的集合 + 版本索引；可以是 git 仓 / 文件系统目录 / vcpkg 内置 baseline。消费方 `vcpkg-configuration.json` 引用 Registry。luban v1.x 候选 verb `luban registry add <git-url>`；v1.0 用户手编 vcpkg-configuration.json。

### 14.3 Blueprint 侧

| 实体 | 字段（关键）|
|---|---|
| `Blueprint` | name / description / [tool.X] / [config.X] / [file."path"] / [env.persistent\|injected] / [capture.X] / meta(requires / conflicts) |
| `Lock` | schema / blueprint_sha256 / tools{platform→{url,sha256,bin,artifact_id}} / files |
| `Tool` | name / version / source / artifact_id / external? |
| `Config` | tool_name / cfg(json) — 由 per-tool Lua renderer（即 `Renderer`）翻译为 dotfile（"挂墙的图"）|
| `File` | target_path / content_sha256 / mode: replace\|drop-in / backup_path? |
| `EnvVar` | name / value(模板) / mode: persistent\|injected — persistent 写 HKCU；injected 仅 luban-spawned 子进程可见 |
| `Capture` | provider（luban 内置白名单，如 `msvc-vcvarsall`）/ mode — 产物按 mode 进 persistent 或 injected env |
| `Generation` | id / created_at / applied_blueprints[] / tools[] / files[] / persistent_env / injected_env — **当前叠层栈的并集快照** |

## 15. 事件 lifecycle

### E1. Blueprint Apply（包含工坊起步）

`luban bp apply X`——X 可以是 `main/cpp-base` 起步，也可以是 user blueprint 名 / 路径。流程详 §11 部署管线（一份图纸，三层产出：Tool 进 store + shim、Config/File 落 XDG、EnvVar/Capture 进 generation env 表）。

幂等：同 artifact-id 已存在跳过 download/extract；shim 总重写；env 表按当前 generation diff HKCU。

### E2. 注册到 HKCU（env --user）

```
luban env --user
   ├─ 读当前 generation 的 persistent_env
   ├─ for each (key, value) in persistent_env:
   │     替换 {{}} 模板 → win_path::set_user_env(key, value)
   ├─ win_path::add_to_user_path(~/.local/bin/)
   │     ├─ 读 HKCU\Environment\PATH（注意 REG_SZ vs REG_EXPAND_SZ）
   │     ├─ 去重 + prepend
   │     └─ 广播 WM_SETTINGCHANGE
   └─ injected_env 不动 HKCU
```

`persistent_env` 内容来自图纸的 `[env.persistent]`（如 cpp-base 的 `VCPKG_ROOT`、cpp-base-wasm 的 `EM_CONFIG`）+ luban 隐式（`LUBAN_PREFIX` 等）。退某层（`bp unapply`）→ 新 generation 不含该层 env → 下次 `env --user` 自动从 HKCU 清掉。

### E3. 项目创建（new）

```
luban new <kind> <name>     # kind ∈ {app, lib, wasm-app, blueprint}
   └─ find_template_root(kind)
   └─ materialize(template, project, {name=<name>, kind=<kind>})
        ├─ walk *.tmpl，剥扩展名
        ├─ {{name}} 在内容 + 目录名都替换
        └─ 非 .tmpl 直接拷贝
   └─ 写 luban.toml [project] kind = "<kind>"（议题 AA 显形）
   └─ kind=lib 时模板默认带 install + export 套路（议题 AA：v1.0 should-have）
   └─ kind ∈ {app, lib, wasm-app}：默认调 luban build（生成 compile_commands.json）
   └─ kind=blueprint：不是 project，写 <config>/luban/blueprints/<name>.lua 模板
```

首次创建后（project 类）：CMakeLists.txt / CMakePresets.json / vcpkg.json / luban.toml / 源文件 **全部归用户所有**；luban 之后只重写 `luban.cmake`（动态部分）+ `luban.toml [project]` 字段在 `target add/rm` 时同步。

### E4. 加 / 删依赖（add / remove）

```
luban add fmt [--version >=10]
   ├─ is_system_tool("fmt") → 否（cmake 等 toolchain 名会被拒）
   ├─ find_project_root()
   ├─ port_lookup("fmt"):
   │     先查 luban.toml [ports.fmt]（用户 hint，议题 AB）
   │     再查 lib_targets.cpp 内置表（upstream port）
   │     都没有 → 报错 + 建议（议题 V：vcpkg 找不到时引导用户写 bp）
   ├─ vcpkg_manifest::add(manifest, "fmt", ">=10")  # vcpkg 原生 version 语法（议题 X）
   └─ luban_cmake_gen::regenerate_in_project(root, targets)
        ├─ 读 luban.toml + vcpkg.json
        └─ render() v1 schema：LUBAN_TARGETS / find_package / luban_apply / luban_register_targets
```

`luban remove fmt` 反向，自动重渲——议题 H 收敛后 `sync` verb 已并入 add/remove 自动。

### E5. 构建（build）

```
luban build
   ├─ 选 preset：look at luban.toml [project] kind:
   │     wasm-app → "wasm" preset（emcmake 包裹）
   │     app/lib + has deps in vcpkg.json → "default" preset
   │     app/lib + no deps → "no-vcpkg" preset
   ├─ env_snapshot::apply_to(env):
   │     PATH = ~/.local/bin/ prepended（toolchain 全经 shim 暴露；不进 PATH，议题 G）
   │     + 当前 generation 的 persistent_env（VCPKG_ROOT 等）
   │     + 当前 generation 的 injected_env（MSVC 的 INCLUDE/LIB 等，仅 luban-spawned 进程可见）
   ├─ proc::run(cmake / emcmake --preset <P>, env=...)
   ├─ proc::run(cmake --build --preset <P>, env=...)
   └─ 同步 build/<P>/compile_commands.json → 项目根
```

### E6. Alias 解析（任何调 alias 的场景）

```
PATH 命中 ~/.local/bin/<alias>.exe = luban-shim.exe（硬链接）
   ├─ GetModuleFileNameW → 自身路径反推 alias
   ├─ 读 ~/.local/bin/.shim-table.json
   └─ CreateProcessW(exe, argv) + 等待 + 透传 exit code
```

文本 shim 走 `.cmd`；`.exe` proxy 依赖 `bp apply` 自动维护的 `.shim-table.json`（不再有独立 `shim` verb，议题 H）。

### E7. 自管理（self update / uninstall / gc）

- **update**：从 GitHub Releases 拉新 `luban.exe`，原地替换；若检测到 v0.x `installed.json` + 无 generation → 自动触发 `migrate`（落地议题 N 的迁移路径）
- **uninstall**：清 `<data>/<cache>/<state>/<config>`，撤 HKCU PATH/env（按当前 generation 的 persistent_env 反向）
- **gc**：清 `<cache>/downloads/` 已下完已解的归档、staging 残留、半截下载（议题 R：与 `bp gc` 不同——bp gc 清 generation/store；self gc 清 luban 自身缓存）

### E8. File Deploy / Backup

`replace` 模式 apply：
1. 计算目标路径 sha256（若已存在）；与 lock 中 content_sha256 比对
2. 不一致 → 备份到 `<state>/backups/<gen-id>/<base64-of-path>`
3. 写新 content
4. 记入 `File` 进 generation

### E9. Generation Switch (Rollback)

```
luban bp rollback [N]
   ├─ 读 generations/N.json
   ├─ for tool: 重建 shim → ~/.local/bin/<tool>
   ├─ for file:
   │     若新 gen 不含此 file → 还原备份
   │     若新 gen 含此 file 且 content_sha256 不同 → 写新内容
   ├─ 重写 HKCU env diff（按新 gen 的 persistent_env）
   └─ 切 current symlink → N
```

## 16. CLI verbs

H/Q/R + AC/AD + AE/AF 收敛后：**22 顶层 verb + bp 6 子命令 + 1 过渡 verb = 29 入口**（cargo / uv / npm / cabal 等范式补全：加 `test` `doc` `check` `tree` `update` `outdated` `search` `clean` `fmt`；复活 `target` + 拆 `gc` 双轨）。

> **1.0 实施状态 (2026-05-05)**：所有 22 顶层 verb + bp 6 子命令 + migrate 已在 `src/commands/`
> 注册并通过 smoke (9/9)。flag 与下表的差异：
> - `bp gc` 已注册但实现 stub 到 v1.x（生成 + store ref-counting + backup expiry 各自 nontrivial）；
> - `tree / outdated` 是 vcpkg 命令的薄包装，输出原样透传未做二次渲染。
>
> v0.x 残留 `shim` / `sync` 仍注册但属内部 plumbing；按 §16.1 决策不暴露教学。

| 组 | verb | 干什么 | forward_rest |
|---|---|---|---|
| **blueprint** | `bp apply <name>` | 在叠层栈顶加一层；接受 `main/cpp-base` / 用户图纸名 / 路径；`--update` 重解析 lock | no |
| **blueprint** | `bp unapply <name>` | 抽掉一层 | no |
| **blueprint** | `bp ls` | 列当前栈里的层 + 各层注册的 resolver/renderer | no |
| **blueprint** | `bp status` | Generation 与磁盘漂移检查（shim / file / env / 注册表覆盖 四方对账）| no |
| **blueprint** | `bp rollback [N]` | atomic 切回前一代 Generation | no |
| **blueprint** | `bp gc` | 清旧 generations + 不被任何 generation 引用的 store 项 + 对应 backup | no |
| project | `new <kind> <name>` | 脚手架——`new app foo` / `new lib bar` / `new blueprint dev` | no |
| project | `build` | wrap cmake configure+build；自动 preset（wasm 走 emcmake）| no |
| project | `add <pkg>` | 加 vcpkg 库 + 自动重渲 luban.cmake | no |
| project | `remove <pkg>` | 反向 | no |
| project | `target add <name> [--lib]` / `target rm <name>` | 在当前 Project 里增删 cmake target；同步 CMakeLists.txt + luban.cmake | no |
| project | `check` | compile-only 快反馈（不 link）；cargo/cabal/pip/gradle 范式 | no |
| project | `test [<filter>]` | wrap `ctest --preset <P>`；透传 filter / regex；不发明 test runner | no |
| project | `doc [--open]` | wrap doxygen；`--open` 自动开浏览器 | no |
| project | `tree` | 项目 vcpkg 依赖树（不同于 `bp ls` 的图纸栈）| no |
| project | `update [--apply]` | 升 vcpkg baseline + 刷 manifest URL；默认 dry-run，`--apply` 提交 | no |
| project | `outdated` | 报告有更新版本可用（不修改），是 `update` 的非破坏性兄弟 | no |
| project | `search <pat>` | 搜 vcpkg port 名 + `lib_targets.cpp` 表（含 find_package + cmake target 映射）| no |
| project | `clean [--cache] [--vcpkg]` | 清 `build/`；`--cache` 扩到 luban 下载缓存；`--vcpkg` 扩到 vcpkg buildtrees | no |
| project | `fmt [<glob>...]` | wrap `clang-format -i` over project sources（如 cli-quality 已带 clang-format 则直接复用 shim）| no |
| env | `env` | HKCU 写入 `--user` / 解除 / `--print` (eval)；写入内容 = 当前 Generation 的 `persistent_env` | no |
| env | `doctor` | 工坊健康（Generation drift / shim 冲突 / lock 失配 / 注册表碰撞 / `--strict` / `--json`）| no |
| utility | `which <name>` | shim/alias → 绝对 exe | no |
| utility | `run -- <cmd>` | 注 env（persistent + injected）后透明 exec | **yes** |
| utility | `describe [port:<name>\|tool:<name>]` | 不带前缀：工坊 + 项目状态（`--json` schema:1 / `--host`）；带 `port:` 前缀显示 vcpkg port + find_package + targets；带 `tool:` 前缀显示 store 路径 + 版本 + alias（议题 AF）| no |
| utility | `self` | `update` / `uninstall` / `gc`——`self gc` 清 luban 自身的下载缓存 / 临时构建目录（≠ `bp gc`）| no |
| utility | `completion <shell>` | bash / zsh / fish / pwsh / clink | no |
| transition | `migrate` | v0.x → v1.0 一次性桥；下个 release 删 | no |

短别名：`luban bp <verb>` ≡ `luban blueprint <verb>`。

**`bp gc` vs `self gc` 分工**：
- `bp gc` 清 blueprint 范畴垃圾——旧 generations、店里没人引用的 store 项、过期 backup
- `self gc` 清 luban 自身垃圾——`<cache>/downloads/` 已下完已解的归档、staging 残留、半截下载

### 16.1 砍掉的入口（H 收敛）

| 砍 | 替代 |
|---|---|
| `setup` | 直接用 `bp apply main/cpp-base [main/cpp-base-wasm/msvc]`——工坊只通过图纸搭，无非图纸入口 |
| `sync` | `add/remove` 自动 sync |
| `shim` | plumbing；`bp apply` 自动维护，不暴露独立 verb |
| `specs` | 删；SAGE / AGENTS.md 与 luban 主线弱关联，独立工具或 fork 出去 |
| `bp install` | 反范式（跳图纸装单工具）|
| `bp init` | → `new blueprint` |
| `bp update` | → `bp apply --update` flag |
| `env --msvc-init` | → `main/cpp-base-msvc.lua` 的 `[capture.msvc]`（provider = `msvc-vcvarsall`）|

## 17. 文件契约

| 文件 | 写者 | 读者 | schema | 兼容策略 |
|---|---|---|---|---|
| `<project>/luban.cmake` | `luban_cmake_gen` | cmake | v1 隐式 | 升级保留旧 marker |
| `<project>/vcpkg.json` | luban + 用户 | vcpkg | vcpkg 自家 | 透传，不发明字段 |
| `<project>/luban.toml` | 用户 + `luban new` | luban | v1 `[project]`（含 `kind`）`[scaffold]` `[ports.X]`（overlay/bridge port hint）| 新增段 optional；`cpp` ∈ {17,20,23}；`kind` ∈ {app, lib, wasm-app} |
| `<config>/emscripten/config` | `main/cpp-base-wasm` 的 `emscripten-cfg` renderer（议题 M）| emcc (via `EM_CONFIG`)| python-style | apply 时重渲 |
| `~/.local/bin/.shim-table.json` | `bp apply` 内部（议题 H）| `luban-shim.exe` | flat map | 同步重写 |
| `<config>/luban/blueprints/<name>.{lua,toml}` | 用户 + `luban new blueprint` | `commands/blueprint` | 1 | 增字段不破坏 |
| `<blueprint>.lock` | `bp apply` / `bp apply --update` | `bp apply` | 1 | 同上 |
| `<state>/luban/generations/N.json` | `bp apply / unapply / rollback` | `rollback` / `doctor` / `bp ls` | 1 | 同上 |

`--json` 约定：所有 `--json` 输出标 `schema: 1`；新字段只能加，不能改名/删。破坏需 ADR。

## 18. 环境变量

环境变量由图纸的 `[env.persistent]` / `[env.injected]` / `[capture.X]` 段声明，apply 时求值并写入 Generation 的两份表（`persistent_env` / `injected_env`）。

### 18.1 Persistent — 写 HKCU，fresh shell 可见

| 变量 | 来源 | 用途 |
|---|---|---|
| `PATH` | luban 隐式（不在 `[env]` 里）| prepend `~/.local/bin/` 一次写入；toolchain bin **不**进 PATH，由 shim 暴露 |
| `LUBAN_PREFIX` | 用户（容器/CI）| 把 4 个 home 都重定向到 `<prefix>/<role>`，`paths.cpp` 读 |
| `XDG_DATA_HOME` 等 | 用户 | XDG-first 解析 |
| `VCPKG_ROOT` | `main/cpp-base [env.persistent]` | 指向 vcpkg 根 |
| `VCPKG_DOWNLOADS / VCPKG_DEFAULT_BINARY_CACHE / X_VCPKG_REGISTRIES_CACHE` | `main/cpp-base [env.persistent]` | vcpkg cache 走 XDG |
| `EM_CONFIG` | `main/cpp-base-wasm [env.persistent]` | 指向 `<config>/emscripten/config` |

### 18.2 Injected — 仅 luban-spawned 子进程可见，不写 HKCU

| 变量 | 来源 | 用途 |
|---|---|---|
| `INCLUDE / LIB / LIBPATH / WindowsSdk* / VCToolsInstallDir`（~30 个）| `main/cpp-base-msvc [capture.msvc]`（provider = `msvc-vcvarsall`）| MSVC vcvarsall capture 注入；fresh cmd.exe 不被污染 |

### 18.3 写入 / 注入路径

- **`luban env --user`**：算当前 Generation 的 `persistent_env` 与 HKCU 的 diff，增删；`injected_env` 不动 HKCU
- **`luban run` / `luban build`** 派生子进程时：`env_snapshot::apply_to(env)` 同时叠 `persistent_env` + `injected_env`
- **退某一层**（`bp unapply <name>`）：新 generation 不含该层贡献的 env → `env --user` 自动清除 HKCU 上的对应项 → fresh shell 立即清爽

## 19. 测试策略

| 层 | 工具 | 范围 |
|---|---|---|
| 单测（leaf 模块）| doctest（vendored 在 `third_party/doctest.h`，ADR-0004）| 一 `tests/test_<module>.cpp` 对应一个 leaf 模块 |
| 端到端 | `scripts/smoke.bat` / `scripts/smoke.sh` | new → add/remove → build → run → target add/rm → test → check → doctor |
| CI 矩阵 | `.github/workflows/build.yml` | windows-latest + ubuntu-24.04 + macos-latest，三平台 gating |

**测试不变量**：

- 不 mock 文件系统——单测用 `std::filesystem::temp_directory_path()`
- 不 mock 网络——下载测试要么走真实小文件、要么 skip
- vcpkg 网络 roundtrip 留 nightly job（smoke 走无 deps 路径）
- `bp apply main/cpp-base` 不在 CI 默认跑（拉 ~250 MB），改 nightly

## 20. 注释规范（hard rule，ADR-0002）

luban 自己的代码必须**详细注释**。具体：

1. **每个 .cpp 顶部**有块注释：模块对外承诺什么 / 为什么是现在这个形态 / 历史演进决策
2. **每个公共函数/类/结构**：一行 doxygen 风格 brief；必要时 `@param` / `@return`；复杂返回类型（含 `expected<T,E>`）必须标 E 的语义
3. **复杂分支 / 状态机 / 边界条件 / workaround** 必须 inline 注释 WHY（不只是 WHAT）
4. **删代码同步删注释**——防 stale
5. **AI agent 输出代码必须配详细注释**，不得省略

反例（不接受）：`// Increment x \n x++;`
正例：`// Pre-decrement so the wraparound case (x == 0) returns INT_MAX, not -1...`

## 21. doctest 选型（ADR-0004）

| 候选 | 单 header | License | 编译时间 | 选择 |
|---|---|---|---|---|
| **doctest** | ✅ ~12k 行单文件 | MIT | 极快 | **选** |
| Catch2 v3 | ❌ 多 lib，需 cmake target | BSL-1.0 | 中 | 拒（不符合 single-header） |
| Catch2 v2 | ✅ | BSL-1.0 | 慢（编译 ~5×）| 拒（停维护） |
| Boost.Test | ❌ boost 子库 | BSL-1.0 | 慢 | 拒（违反"无 boost"）|
| GoogleTest | ❌ 多源 + lib | BSD | 中 | 拒（不符合 single-header）|

doctest 优势：header-only / 编译时间 ~3× fastest / API 跟 Catch2 几乎一样（`TEST_CASE` / `CHECK` / `REQUIRE` / `SUBCASE`）/ MSVC + clang + gcc + mingw 都验证过。

vendor 政策：`third_party/doctest.h`；版本从 GitHub releases 取最新 stable；升级走 PR；禁本地 patch（bug 走上游修）。

## 22. POSIX 跨平台（ADR-0006）

| Phase | 范围 |
|---|---|
| **A** Entry gate | luban builds clean on Linux/macOS（stub 仍是 stub） |
| **B** 关键路径 | hash.cpp 走 OpenSSL EVP；download.cpp 走 libcurl |
| **C** Toolchain habits | `~/.bashrc` PATH / symlink shim / vcpkg triplet 自动选 / `install.sh` |

**out of scope**：cross-target build / MSVC on POSIX。

## 23. 主要决策与理由

### luban 自身重写 / vendor / FFI（ADR-0001 + 0002）

**luban.exe 自身（scope 1）**优先级：重写 → vendor single-header → 否决 FFI。

- 重写：~6.5k 行 C++23（cli / paths / proc / download / hash / archive / shim / win_path / blueprint_apply / lua_engine / extension_registry 等 ~20 个模块）
- vendor single-header：`third_party/{json.hpp, miniz.{h,c}, toml.hpp, doctest.h}` + `lua54/`（QJS 是 v1.x 议题 T，本期不 vendor）
- FFI 不允许：不嵌 Rust crate / DLL / .so / .dylib

**luban-managed packages（scope 2，blueprint 装的工具）**优先级：vcpkg 命中走 vcpkg；vcpkg 缺货 → blueprint 直接下 GitHub release 资产；不需要 luban registry（ADR-0005/0007 已撤回）。

### 8 不变量被拒的方案（ADR-0001）

- **A xmake DSL → lower 到 cmake**：用户最终仍需懂 cmake，逃生通道粗糙
- **B CMakeLists.txt 内 marker block 编辑**：边界脆弱，hand-edit 易冲突
- **C unified `luban.toml`（cargo 风格）**：与 vcpkg.json 双重真相
- **D `luban-init.exe` bootstrapper**：luban.exe 本身已是单文件，多一层无价值

### Blueprint 模型决策

| 决策 | 理由 |
|---|---|
| 嵌 Lua 5.4 而非 Python | Python 嵌入 ~10 MB+ 破 R1 单 binary；Lua ~200 KB 纯 C 可 sandbox |
| Lua DSL 主用，TOML 定形（QJS 推迟到 v1.x）| Lua 表语法是 DSL sweet spot（home-manager / wezterm 验证过）；用户原话"Lua 写 DSL 真的很爽"；TOML 主管 shape 契约 |
| `~/.local/bin/` 而非 `<data>/bin/` | 与 uv/pipx/claude-code 共享 XDG bin home；用户已习惯 |
| 内置图纸主用 Lua | 平台分支 / 路径合成 / 注册扩展函数都需要表达力；TOML 仅承载极静态 |
| vcpkg 不被 luban registry 替代（撤回 ADR-0005/0007）| blueprint 模型解决"个人 toolset"是另一面；不与 vcpkg 抢地盘 |
| Per-tool Lua renderer 而非 generic 模板 | home-manager 实践：每工具一份模块更可维护；加新工具 = 加 .lua 文件不改核心 |
| 不变量 1 放宽（Lua 例外） | single-header 仍是默认；多文件需有充分理由 |
| AGENTS.md marker block 保留（与 blueprint 正交） | luban → AI agent 传话机；不被 blueprint 模型替代 |
| 静态链接 luban.exe | 新装 Win10 无 PATH 即可跑；不变量 7 |

## 24. 开放设计议题

近期讨论沉淀。**已决**项作设计参考；**开放**项待拍板。

### 24.1 已决（字母序）

| ID | 议题 | 决定 | 实施状态 |
|---|---|---|---|
| **A** | 叠层冲突解决 | last-applied wins + warn（同 nix profile / home-manager 习惯） | — |
| **B** | Tool 重要度是否进 schema | 不进——doctor / status 只汇报"图纸列出的 Tool 是否就位"，不发明优先级 |
| **C** | Configs 多层共存 | 每层独立 drop-in（`~/.gitconfig.d/<bp>.gitconfig`）；用户顶层 `[include]` 拉整目录 |
| **D** | Env 写哪、怎么写 | 图纸 `[env.persistent]` 写 HKCU；`[env.injected]` 仅 luban-spawned 子进程；`[capture.X]` 走内置 provider 白名单（如 `msvc-vcvarsall`）。Generation 持 persistent_env / injected_env 两份表 |
| **E** | 模板引用语法 | `{{ x.y.z }}` —— chezmoi 风格；与 shell `${X}` 区分；语法**受限**（点路径，无 if/for/拼接）；想要表达式请写 Lua |
| **F** | 内置 blueprint 命名空间 | `embedded:<X>` 保留前缀——一眼看出是 luban 自带还是用户写；用户图纸不能用此前缀 | **撤场 (AG, 2026-05-06)**：v1.0 已无内嵌 bp；`embedded:<X>` 改硬错误，引导走 `bp src add` |
| **G** | PATH 管理边界 | **shim-only 不破例**——`~/.local/bin/` 永远 persistent；toolchain bin **绝不**进 PATH，全靠 shim 暴露。哪怕图纸内的某个特殊工具想直接 PATH 露出，答案是"加个 shim 行不行？"——保护"PATH 干净"这条不变量 |
| **H** | 动词收敛 | **结案**——详 §16；29 入口；多次拍板 trace：Q / R / AC / AD / AE / AF |
| **J** | 图纸 schema 与三种壳的角色 | **TOML 形态主管 shape 契约**（字段 + 层级 + 类型）；**Lua DSL 主推输入形态**（内置图纸主用）；JS 兜底；三者 fold 到同一 BlueprintSpec |
| **L** | Blueprint 携带逻辑的形态 | **inline**——spec + 扩展函数（resolver / renderer）同在一个 .lua 文件；TOML 只能引用，不能注册；luban 内置走同一 `register_*` API（无双码路径） |
| **M** | 扩展机制交付时间线 | **v1.0 必交**——blueprint 模型完整性的一部分；不留 v1.x。vcpkg bootstrap / emscripten config 这两个 component.cpp 硬编码特例必须借此机制收编 | **大部分落地 2026-05-05**：（a）`post_install` hook 已落地（`[tool.X.post_install]` 解析 + apply 阶段路径穿越守卫 + cmd /c / sh 包装 + cache 命中跳过）；（b）vcpkg bootstrap 特例已通过 source-zip fallback + post_install 收编进 `main/cpp-base`，无 C++ 硬编码；（c）C++ 侧 resolver hook 仍以 `source_resolver_github.cpp` per-scheme TU 为模板；（d）Lua-side `luban.register_resolver/renderer` 因 `program_renderer` 单 Engine 寿命问题挪到 v1.1（详 docs/FUTURE.md 同名条目）|
| **N** | `setup` 是否保留为 sugar | **删**——工坊只通过图纸搭，无非图纸入口；`bp apply main/cpp-base` 是唯一路径；onboarding 文档把这一句话写清楚就够 | **已落地 2026-05-05**：`src/commands/setup.cpp` + `templates/help/setup.md` removed |
| **O** | `specs` (SAGE / AGENTS.md) 处置 | **删**——与 luban 主线弱关联；想保留这套能力可独立做工具或从老代码 fork 出去 | **已落地 2026-05-05**：`src/commands/specs.cpp` + `templates/help/specs.md` removed; `marker_block.cpp` 留着但失去引用，可后续清理 |
| **P** | "Program" 名字别扭 + L2/L3 关系隐式 | **改名（单数）+ 显式绑定**：`[programs.X]` → `[config.X]`、`[tools.X]` → `[tool.X]`、`[files."path"]` → `[file."path"]`（TOML 习惯单数）；模块 `program_renderer` → `config_renderer`；目录 `templates/programs/` → `templates/configs/`。新增可选字段 `[config.X] for_tool = "Y"` 显式声明该 config 配的是哪个 tool（缺省 = X，schema 化此前隐式的同名约定）| **概念已落 2026-05-06**：CLAUDE.md "Concepts" 节 + DESIGN §9.0 写明三件概念 + 单数命名 + `for_tool` 语义。**代码 rename 待独立 PR**：影响 blueprint_toml 解析、blueprint_lua 解析、3 张 embedded TOML、5 个 .lua renderer 路径、CMakeLists embed foreach、所有相关测试 + 调用点；预计一次性切换不留 alias |
| **Q** | `target` verb 是否复活 | **复活**——`target add` / `target rm` 同步 CMakeLists.txt + luban.cmake；不留删 target 的洞 |
| **R** | `gc` 拆双轨 | **拆**——`bp gc` 清 blueprint 范畴垃圾（旧 generation / 无引用 store 项 / 过期 backup）；`self gc` 清 luban 自身垃圾（download cache / staging）|
| **S** | Capture 是否独立 noun | **保留**——schema 段 `[capture.X]` 显形；不并入 EnvVar 的子段（避免四级嵌套 `env.captured.X` 别扭）|
| **T** | QuickJS 实现与设计时间线 | **delay 到 v1.x**——v1.0 只做 TOML + Lua 两壳；QJS 引擎、`blueprint_qjs.cpp` / JS 形态相关设计本文档不展开；不扩张 quickjs 范围 |
| **U** | "添加"语义被多 verb 瓜分（add / target add / new / bp apply）| **维持各管各**——每个动词专管一件事；不做 brew install 那种 "add 全能" dispatch（脆弱、错误提示难精准）|
| **V** | `luban add <pkg>` 在 vcpkg 找不到时 | **报错 + 建议**——告诉用户 "如果想装 PATH 上的 CLI 工具，请加到一份 user blueprint"；不自动路由到 blueprint（避免 magic dispatch）|
| **W** | 是否给"加 tool 到 user blueprint"开快捷 verb | **不开**——保留"图纸是 git-trackable 的人写文件"性质；快捷入口写图纸会侵蚀用户对图纸的整体掌控（同 ADR-0001 拒绝编辑 CMakeLists.txt 理由）|
| **X** | `luban add --version X` 的语义 | **对齐 vcpkg 原生**——写 vcpkg-features 风格的 baseline 约束（`version>=`）；不发明 luban-specific version 语法 |
| **Y** | "Build 出的制品"叫什么 | **Output**——cmake/cargo/bazel 都用 output；与 Target（声明）形成"声明 vs 实物"对偶。luban 不管 Output 生命周期（cmake build dir 自管） |
| **Z** | vcpkg / 自打 / Rust wrap 三类如何统一 | **Port**（扩 vcpkg 术语）——Upstream port / Overlay port / Bridge port 三类；下游对 `find_package` 无差别。Bridge port 脚手架推到 v1.x |
| **AA** | Project kind 是否显形 | **显形**——`luban.toml [project] kind = "app" \| "lib" \| "wasm-app"` 固化主类型；`luban port new --from-project` / doctor / build 按 kind 分支；scaffold 可重现；`luban new lib` 默认带 install + export 套路（v1.0 should-have）|
| **AB** | overlay/bridge port 的 cmake target hint 怎么给 | `luban.toml [ports.X] find_package = "..." targets = [...]`——`add` 时 luban.cmake 重渲先查 `[ports.X]` 再回退到内置 `lib_targets.cpp` 表 |
| **AC** | `luban test` 是否加入 | **加入**——cargo test 范式平移；薄壳：`ctest --preset <P> [-R <filter>]`；不发明 test runner |
| **AD** | `luban doc` 复活（之前砍是错的）| **复活**——cargo doc 范式同样是 wrapper（rustdoc / doxygen 各取一）；薄壳：跑 doxygen，`--open` 开浏览器 |
| **AE** | cargo / uv / npm 范式补全（跨 8 ecosystem 验证后的高频 verb）| 加 6 个：`check`（compile-only）/ `tree`（vcpkg 依赖树）/ `update`（升 baseline + manifest，dry-run by default）/ `outdated`（非破坏性报告）/ `search`（搜 vcpkg port + find_package 映射，> `vcpkg search`）/ `clean`（build dir，`--cache/--vcpkg` 扩范围）。False positives 验证后**不加**：publish / login / switch / pin / init / exec / vendor / bench / audit / link / lock / cache（已折进 clean 的 flag） |
| **AF** | `fmt` + describe 的 introspection 扩展 | **加 `luban fmt`**：薄壳 clang-format（cli-quality 提供 clang-format shim 时直接复用）。**扩 `describe`** 接 `port:<name>` / `tool:<name>` 前缀做 dep/tool 内省（`luban describe port:fmt` 显示 vcpkg port + find_package + targets；`luban describe tool:cmake` 显示 store 路径 + 版本 + alias），免另开 `show` verb |
| **AG** | Blueprint 分发：embedded vs source | **embedded 全退场，`bp source` 取代**——参 §9.10。luban.exe **零内嵌 bp**——基础 3 件（git-base / cpp-base / cli-quality）都从外部 source 拉。命名复用 luban schema 已有 `source` 一词（tool 层已用，bp 层平行扩展）；模型对照 scoop bucket / brew tap / vcpkg registry。理由：用户的"本机只装 luban、bp 全在 repo"模型与任何 embed 直接矛盾；source 提供分发 / 更新 / 共享 / 版本固定的一等公民机制；离线场景靠 file:// + 本地 clone 解决，不烤 bootstrap snapshot（避免"全 source 化"是假的）。 | **2026-05-06 终态落地**：`bp src add/rm/ls/update` + `bp search` + `<source>/<bp>` 解析 + 同名 add 覆盖 + URL shorthand / 自动 name 推导；lock schema 加 `bp_source/bp_source_commit`；`embedded:<X>` 改硬错误；`templates/blueprints/*.toml` 删除（移到 https://github.com/Coh1e/luban-bps）；`src/main_source.{cpp,hpp}` 删除；CMakeLists 删 embed 逻辑。git clone 模式 + 签名验证 v1.x 跟进。 |

### 24.2 开放

（暂无——上轮讨论的设计议题全部收口；剩余开放问题属实现侧，待落地时拍。）
