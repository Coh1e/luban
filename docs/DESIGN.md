# luban

## 1. 一句话

**luban：给一张图纸，搭一座 Windows-first 的 C++ 工坊。**

luban 不替代 CMake，不替代 vcpkg，不做通用包管理器。
它负责把 C++ 开发所需的工具链、基础 CLI、配置、环境变量和项目胶水一次性搭好，并保持可解释、可卸载、低侵入。

------

## 2. 北极星

1. **CMake 是主体**：luban 不发明 build IR。
2. **项目库依赖仍属于 CMake / vcpkg / FetchContent 等原生机制**：luban 不另建 `[deps]`。
3. **Toolchain 与 project library 分层**：工具链是工坊能力，项目库是项目事实。
4. **Windows-first，零 UAC，用户级写入**。
5. **PATH 只暴露 `~/.local/bin`**，具体工具目录不进入全局 PATH。
6. **Lua 是 primary frontend，TOML 是静态 projection**。
7. **不做通用软件安装器**：浏览器、Office、媒体软件、系统级运行时都不是 luban 范围。
8. **不劫持 CMake 工作流**：luban 只便利，不接管。生成的 CMake 内容应可脱离 luban 使用。

------

## 3. MVP 范围

MVP 只做三件事：

### 3.1 搭工坊

通过远端 blueprint snapshot 安装和配置 C++ 开发工坊。

概念上分三层（但实际官方 bp 已合并打包，见下）：

```text
bootstrap layer:
  git / mingit / gcm / 必要配套

toolchain layer:
  llvm-mingw / cmake / ninja / vcpkg
  未来可增加 msvc build tools / windows sdk

workbench layer:
  fd / ripgrep / PowerShell 7 / psreadline / zoxide / starship / Windows Terminal 配置 / 字体与主题配置
```

实际官方 bp 表面（`Coh1e/luban-bps`）：

```text
bootstrap     bootstrap layer + toolchain layer 合并（mingit + gcm + lfs +
              llvm-mingw + cmake + ninja + vcpkg）。两层在实际安装中没人
              只装其一，分开只增加 UX 摩擦。

onboarding    workbench layer + 字体 / WT 主题 / pwsh profile 全部并入。
              requires bootstrap，apply 时自动按依赖顺序 trigger。
```

说明：

- Git 是 fresh host 的第一个刚需能力。
- fresh host 不假设有 git、curl、wget。
- luban 初次安装由用户手动下载 luban.exe（提供类scoop一键脚本）。
- luban 安装后应给出明确 post-install 提示：下一步应用 bootstrap blueprint。
- blueprint source 永远是远端 snapshot，不是 git clone。
- 不要本地 blueprint 设计。
- 无 scoop bucket 概念。

------

### 3.2 管项目依赖

luban 的项目价值集中在：

```text
luban add
luban remove
```

它们负责低心智负担管理 C++ 项目依赖：

- 编辑 `vcpkg.json`；
- 维护 luban 生成的 CMake 胶水；
- 对简单项目自动接线；
- 对复杂项目给出明确 CMake 使用片段；
- 不把 CMakeLists.txt 变成 luban 私有格式；
- 不发明新的依赖 manifest。

------

### 3.3 解释、诊断、卸载

luban 必须能说清楚自己做了什么：

```text
luban describe
luban doctor
luban self uninstall
```

所有 destructive 操作都支持：

```text
--dry-run
```

------

## 4. 核心名词 Nouns

### Blueprint

一张远端图纸，描述一种工坊能力。

Blueprint 可以声明：

```text
tool
config
file
env
capability
post-install action
```

Blueprint 用 **Lua DSL** 描述。
TOML 只作为静态 projection，不作为主要配置入口。

------

### Source Snapshot

远端 blueprint 仓库在某一时刻的快照。

luban 不依赖本机 git 拉取 blueprint。
source update 的语义是重新下载远端 snapshot。

------

### Tool

被 luban 安装或发现的可执行工具。

Tool 进入 luban 管理的用户级 store，并通过 shim 暴露到：

```text
~/.local/bin
```

如果系统里已有同名工具，luban 可以 external skip，但必须提醒用户：

```text
这个工具来自外部环境，可能降低可复现性。
```

------

### Config

工具配置。

Config 必须通过 Lua DSL 描述，由 renderer 生成实际配置文件。

Renderer 不能任意写路径。
它必须声明 capability：

```text
可写哪些目录
是否覆盖已有文件
是否需要用户确认
是否会修改 Windows Terminal / PowerShell profile / 字体配置
```

------

### Env

环境变量分两类：

```text
persistent env:
  写入用户级环境，供普通 shell / IDE 使用

session env:
  只在 luban 启动的单次会话中注入
```

MSVC 相关环境变量属于 session env。

------

### Capability

Blueprint 的权限声明。

apply 前 luban 必须展示 trust summary：

```text
将安装哪些工具
将写哪些文件
将修改哪些 env
将添加哪些 shim
是否会运行 post-install action
是否会修改 Windows Terminal / PowerShell profile
是否来自官方 source
```

非官方 blueprint 必须红色警告。
官方 blueprint 默认信任，但仍展示 summary。

------

### Apply Receipt

luban 内部记录的一次 apply 结果。

它用于：

```text
describe
doctor
uninstall
dry-run diff
后续可复现
```

MVP 文档不暴露 lock 这个概念。
如果需要实现，可以内部叫 lock / receipt / resolved state，但用户心智里只有 blueprint 和当前状态。

------

### Current State

当前机器上的 luban 工坊状态。

记录：

```text
已应用哪些 blueprint
安装了哪些 tool
写了哪些 config/file
设置了哪些 env
哪些 tool 是 external skip
哪些 capability 被授权过
```

MVP 先不做 rollback，所以不需要 generation history。
未来做 rollback 时，再把 current state 扩展为 generation。

------

## 5. 核心动词 Verbs

### 工坊

```text
bp source add
bp source update
bp list
bp apply
bp apply --dry-run
```

含义：

- 添加远端 blueprint source；
- 下载 source snapshot；
- 查看可用 blueprint；
- 应用 blueprint；
- dry-run 展示将发生什么。

不支持：

```text
embedded blueprint
local blueprint
bucket
tool install
```

------

### 项目

```text
new
add
remove
build
run
```

含义：

- `new`：创建最小 C++ 项目；
- `add` / `remove`：添加/移除项目库依赖（编辑 `vcpkg.json` + `luban.cmake`）；
- `build`：调用 CMake 构建；
- `run`：把 luban 的 toolchain env（PATH + VCPKG cache 等）注入子进程后
  exec 任意命令（uv 风格透传）。`<cmd>` 在 luban-augmented PATH 上做
  PATH 搜索；`<cmd>` 后所有参数原样转发给 `<cmd>`，luban 自己不解析。
  适用场景：未跑 `luban env --user` 的 fresh shell / CI / 容器。

```text
luban run cmake --version
luban run vcpkg list
luban run clang -E -dM -x c nul
```

历史注：v0.x 时设计的 `run` 是 build-artifact runner（"运行默认 target
或指定 target 的构建产物"），但实际工程中 `cmake --build && ./build/...`
已经覆盖该需求；v1.0 把 `run` 收敛为 PATH-augmented exec，给"luban 不
长期占据 HKCU 但要 fresh shell 跑工具链"这个场景一个直接入口。

------

### 环境与解释

```text
env
describe
doctor
```

含义：

- `env`：打印或应用用户级环境变量；
- `describe`：解释当前工坊、项目、工具、env、风险来源；
- `doctor`：检查漂移、冲突、缺失工具、外部工具、非官方 blueprint 风险。

------

### 自管理

```text
self update
self uninstall
self uninstall --dry-run
```

`self uninstall` 删除干净：

```text
luban store
luban cache
luban state
shim
用户级 env
PATH 中的 ~/.local/bin 项
```

但必须先 dry-run 展示将删除什么。

------

## 6. MSVC 设计边界

MSVC 不是普通 toolchain layer 的简单变体。

它包含两件事：

### 6.1 静默安装

未来的 MSVC blueprint 可以负责：

```text
MS Build Tools
Windows SDK
必要组件选择
用户级记录安装状态
```

但它不安装 cmake、ninja、vcpkg 等通用工具。

### 6.2 单次会话 env 注入

luban 提供专用动词把 `vcvarsall.bat` 产生的环境变量带入单次会话：

```text
luban msvc shell
luban msvc run -- <cmd>
```

`luban env --msvc-init [--arch x64]` 一次性捕获 vcvarsall 输出到
`<state>/msvc-env.json`；之后 `luban msvc shell` / `msvc run` 读这份
缓存把 ~30 个 INCLUDE / LIB / WindowsSdk* 等变量注入子进程；`luban env
--user` 则把它们写入 HKCU（仅 PATH 第一项，避免 ~14 条 SDK 子目录污
染用户 PATH）。

走 `msvc` 专用动词而不是 `run --msvc`：MSVC env 是 ~30 条变量加 ~14 条
PATH 项的大块结构，独立 verb 让 help 与 examples 一目了然，避免 `run`
的 forward_rest 边界跟 `--msvc` flag 解析互绞。

这些 env 不写入 HKCU（除非用户显式 `env --user`），不污染全局用户
环境。

------

## 7. 配置层 MVP 内容

workbench layer 可以覆盖：

```text
fd
ripgrep
PowerShell 7 portable install
PSReadLine
用户提供的 PowerShell profile
zoxide
starship
Windows Terminal profile
Maple Mono NF CN 字体用户级安装
Windows Terminal 默认字体
Ayu Mirage theme
```

要求：

- 配置必须通过 Lua DSL；
- PowerShell profile 来自人类提供的内容，luban 负责放置和接线；
- Windows Terminal 不存在时不创建强依赖，不报硬错误；
- 字体用户级安装；
- 所有写文件行为进入 capability summary；
- 所有覆盖行为必须可 dry-run。

------

## 8. 信任模型

官方 blueprint：

```text
默认信任
仍展示 summary
```

非官方 blueprint：

```text
红色警告
显示来源
显示将执行的 capability
要求用户确认
```

普通 GitHub source 默认信任上游，不在 MVP 做强 SHA256/签名验证。

诊断责任分工（MVP）：

`doctor` 报：

```text
此 blueprint / source 来源非官方
此 tool 使用 TOFU / 未验证来源（lock 里 sha256 为空）
```

`bp apply` 的 trust summary 报：

```text
此 tool 是 external_skip（probe target + 是否命中本机 PATH）
此 config 的 renderer 声明的 capability（写哪些目录 / 是否覆盖 / ...）
此 bp 的 post_install hook
```

外部 skip 的检测放在 apply 时而不是 doctor，是为了把 doctor 的链接面收
窄到长期状态（sources.toml / lock 文件）；apply 已经持有完整的 spec，能
逐 tool 报告 probe 命中情况，重复劳动反而稀释信号。

------

## 9. 下载与单二进制

luban 初次安装不必须依赖 install.ps1。

用户可以手动下载 luban.exe。
之后 luban 自己具备下载能力（v1.0.7+：in-process libcurl + Schannel + HTTP/2 强制；
FetchContent 在 luban 构建时拉源码，最终二进制全静态链）。

实现目标：

```text
单二进制（MSVC /MT 静态链）
零 UAC
不依赖 host wget / git
下载语义由 luban 自己提供
```

POSIX 不做（v1.0.5）。luban 是 Windows-first 的工坊启动器（§1）；
macOS / Linux 用户 brew / apt / nix 的 C++ 工具链体验已经足够好，
luban 不去填那个不存在的洞。

------

## 10. 测试要求

MVP 必须有：

```text
unit tests
e2e tests
```

重点覆盖：

```text
blueprint apply dry-run
tool install / external skip
config capability summary
env 写入与撤销
vcpkg.json add/remove
luban.cmake 生成
describe --json
self uninstall --dry-run
非官方 blueprint 警告
```

------

## 11. 暂不做

MVP 删除或推迟：

```text
rollback
generation history
embedded blueprint
local blueprint
bucket
tool install
target add/rm
registry mirror
TUI
多版本共存
官方 IDE 插件
通用应用安装
包管理器目标
复杂 source trust / sigstore / sumdb
```

------

## 最终收敛后的定位

luban MVP 应该是：

```text
一个 Windows-first 的 C++ 工坊启动器 + 依赖便利层 + 可解释环境管理器。
```

不是：

```text
通用包管理器
CMake 替代品
vcpkg 替代品
shell 美化框架
Windows 软件安装器
Nix/Home Manager 完整复刻
```

最核心的一句话可以改成：

> **luban 用远端 Lua blueprint 搭建 C++ 工坊，用 vcpkg/CMake 原生机制管理项目库，用 describe/doctor 保证一切可解释、可卸载、低侵入。**
