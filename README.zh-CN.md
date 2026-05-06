# Luban (鲁班)

[![license](https://img.shields.io/badge/license-MIT-blue)](LICENSE)
[![English](https://img.shields.io/badge/English-README-lightgrey)](README.md)

**给一张图纸，搭一座 C++ 工坊。**

luban 读一份 Lua / TOML 图纸（blueprint），照图纸把这台机器的 C++ 工作面装齐：
工具链（LLVM-MinGW + cmake + ninja + mingit + vcpkg）、CLI 利器（ripgrep / fd /
bat / ...）、配置文件（git / bat / fastfetch / ...）。多张图纸像 layer 一样叠，
可原子 rollback。

单一静态链接二进制，零 UAC，XDG-first，内嵌 Lua 5.4。Windows-first；
POSIX 可构建。

每个 release 出**两个 flavor**，都 static-linked：`luban-msvc.exe`（~3 MB，
默认；MSVC `/MT`）和 `luban-mingw.exe`（~6 MB；LLVM-MinGW `-static`）。CI 用
`dumpbin` / `llvm-readobj` 校验 invariant 7（无 vcruntime / msvcp / libgcc_s /
libstdc++ 依赖）。

## 安装

### Windows（PowerShell，推荐）

```pwsh
irm https://github.com/Coh1e/luban/raw/main/install.ps1 | iex
```

安装器把 `luban.exe` + `luban-shim.exe` 落到 `~/.local/bin`（和 uv / pipx /
claude-code 共享同一个 XDG bin 目录），用最新 release 的 `SHA256SUMS` 校验，
然后提示你跑 `luban env --user`（把 `~/.local/bin` 和 toolchain bin 注册到
HKCU PATH）。已装过的机器重跑只 SHA 对比，不会重复下载。

#### 调参 env vars

| env | 默认 | 作用 |
|---|---|---|
| `LUBAN_INSTALL_DIR` | `~/.local/bin` | 安装目标目录 |
| `LUBAN_FLAVOR` | `msvc` | 选哪个 flavor (`msvc` \| `mingw`) |
| `LUBAN_FORCE_REINSTALL` | unset | =1 时跳过 SHA 命中短路 |
| `LUBAN_GITHUB_MIRROR_PREFIX` | unset | 反代前缀（如 `https://ghfast.top`），CN/SEA 慢网用；installer 和 luban 都认。**注意公共 mirror 限速严，能直连优先直连** |
| `LUBAN_PARALLEL_CHUNKS` | `1` | bp apply Range 并发数（默认单流，最高 16）。GitHub release CDN 对多并发 per-IP throttle 很狠（VN 实测 1 路 4.7 MB/s，4 路反而掉到 150 KB/s），单流才是常态。私有 S3/内网镜像才调高 |
| `LUBAN_PROGRESS` / `LUBAN_NO_PROGRESS` | unset | 强制开/关进度条（TTY 默认开） |

### 手动

从 [最新 release](https://github.com/Coh1e/luban/releases/latest)
按需下载 flavor，落地时改回不带后缀的名字：

```pwsh
gh release download --repo Coh1e/luban `
  -p luban-msvc.exe -p luban-shim-msvc.exe -p SHA256SUMS
sha256sum -c SHA256SUMS
Rename-Item luban-msvc.exe      luban.exe
Rename-Item luban-shim-msvc.exe luban-shim.exe
```

放进 PATH 上的目录（比如 `~/.local/bin`）。

### 更新 / 卸载

```pwsh
luban self update                              # 拉最新 release，原子替换 binary
luban self uninstall --yes                     # 完全卸载（binary + dirs + env）
luban self uninstall --yes --keep-toolchains   # 只清 bp 状态，留 cmake/vcpkg
```

`self uninstall` 走 **ownership gate**：HKCU env 变量（VCPKG_ROOT、EM_CONFIG、
msvc captures、PATH entries）只有在指向 luban 自己 dir 内时才会被 unset；
指向外部的会保留并打 log——luban 不会破坏你手工配的环境。

## 快速上手

`install.ps1` 装 luban 时已经自动 `bp src add Coh1e/luban-bps` + 预装
`main/foundation`（git + ssh + lfs + gcm — 几乎其他每张图纸的真前置，无 prompt），
并询问是否装 `main/cpp-toolchain`。装完后：

```pwsh
# 可选额外 bp（installer 不预装的）
luban bp apply main/cli-tools          # zoxide / starship / fd / ripgrep
luban env --user                       # 注册 HKCU PATH；开新终端立即生效

# 创建 + 构建项目
luban new app hello && cd hello
luban add fmt && luban build
```

luban 二进制**零内嵌 bp**。基础 4 件（`foundation` / `cpp-toolchain` /
`cli-tools` / `onboarding`）住在外部
[Coh1e/luban-bps](https://github.com/Coh1e/luban-bps)。任何人都能发自己的
blueprint source repo 然后 `luban bp src add` 注册。

### 图纸 schema（v0.2.0）

```toml
schema = 1
name = "my-bp"

[tool.ripgrep]                          # 一个 tool 一个二进制（落到 PATH）
source = "github:BurntSushi/ripgrep"

[config.git]                            # 通过 git renderer 渲染 dotfile
userName = "alice"

[file."~/.config/starship.toml"]        # 直接落一个文件
mode = "merge"                          # replace | drop-in | merge | append
content = '{"add_newline": false}'

[meta]
requires = ["main/foundation"]          # 强制——apply 顺序显式可读
```

单数 key（`tool` / `config` / `file`）—— v0.2.0 schema rename（议题 P）
把旧的复数形式弃了；老 bp 需要切到单数。

## 它是什么

C++ 在 Windows 上原本要装的散件（toolchain / cmake / ninja / clangd / vcpkg /
git + 一堆 cmake 胶水代码）合在一起几天的活，luban 把它做成 **多层叠图纸**：

- **承重墙**：`main/cpp-toolchain` 装编译器 / cmake / vcpkg / 等（依赖 `main/foundation`）
- **工作台**：`main/cli-tools` 装日常 CLI 利器（zoxide / starship / fd / ripgrep）+ dotfiles
- **装修**：用户写自己的 bp source repo 加私有工具与配置（onboarding 风格）

之后每个项目里：

```pwsh
luban new app foo                    # 脚手架 + 自动 build，clangd 立即可用
cd foo
luban add fmt                        # 写 vcpkg.json + 重渲 luban.cmake
luban build                          # cmake/vcpkg manifest 自动取依赖
build\default\foo.exe                # hello from foo!
```

**用户全程不打开 `CMakeLists.txt`、不写 `find_package`、不读 vcpkg manifest mode 文档。**

## 它不是什么

- **不是构建系统**——cmake + ninja 仍然负责构建
- **不是包管理器**——vcpkg 仍然负责 C++ 包解析与构建
- **不是替代品**——luban-managed 项目仍然是**标准 cmake 项目**，clone 到没装
  luban 的机器上 `cmake --preset default && cmake --build --preset default` 照样能 build

luban 是**带主见的胶水**——把对的工具用对的方式粘起来，给初学者**默认就走在轨道上**的体验。

## 命令清单（34 入口，5 组）

| 组 | 命令 |
|---|---|
| blueprint | `bp apply` `bp unapply` `bp ls` `bp status` `bp rollback` `bp gc` `bp search` `bp src add/rm/ls/update` |
| project | `new` `build` `check` `test` `doc` `add` `remove` `target add/rm` `tree` `update` `outdated` `search` `clean` `fmt` |
| env | `env` `doctor` |
| utility | `which` `run` `describe` `self` `completion` |
| transition | `migrate`（v0.x 桥，下个 release 删）|

完整说明见 `luban <verb> --help` 与 [`docs/DESIGN.md`](./docs/DESIGN.md) §16。

## 设计哲学（一行版）

cmake 永远是项目主角；luban 是**胶水**。工坊用图纸搭，可逆叠层；项目里
luban 只写一份 `luban.cmake`（标准 cmake module）+ 编辑 `vcpkg.json`——
两个文件，git-tracked，删掉 luban 项目仍能 build。

完整 8 条架构不变量见 [`docs/DESIGN.md`](./docs/DESIGN.md) §6。

## License

luban 自身：[MIT](LICENSE)。Vendored 第三方：
- `third_party/json.hpp` — nlohmann/json，MIT
- `third_party/miniz.{h,c}` — Rich Geldreich，BSD-3-Clause（`third_party/miniz.LICENSE`）
- `third_party/toml.hpp` — Mark Gillard 的 toml++，MIT（`third_party/toml.LICENSE`）
- `third_party/doctest.h` — Viktor Kirilov，MIT（`third_party/doctest.LICENSE`）
- `third_party/lua54/` — Lua 5.4，MIT（`third_party/lua54/LICENSE`）
- `third_party/quickjs/` — QuickJS-NG，MIT
