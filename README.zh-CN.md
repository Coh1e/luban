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

### 图纸 cookbook

图纸住 `<bp-source>/blueprints/<name>.{toml,lua}`。单数 TOML key
（`tool` / `config` / `file`）是 v0.2.0 schema（议题 P）。下面都是 TOML
形态；Lua 形态参考 `Coh1e/luban-bps/blueprints/onboarding.lua`。

#### 1. 最简单——把工具落到 PATH

```toml
schema = 1
name = "ripgrep"
[tool.ripgrep]
source = "github:BurntSushi/ripgrep"
```

`luban bp apply <src>/ripgrep` → 拉最新 release zip → 解压 → shim
`rg.exe` 到 `~/.local/bin/`。Asset scorer 按 host triplet 选 zip；sha256
首次 apply 后 pin 到 `<src>/blueprints/ripgrep.toml.lock`。

#### 2. 多二进制工具——显式 shim 列表

```toml
[tool.openssh]
source = "github:PowerShell/Win32-OpenSSH"
bin = "ssh.exe"
shims = ["ssh.exe", "ssh-keygen.exe", "ssh-agent.exe", "scp.exe", "sftp.exe"]
external_skip = "ssh.exe"      # PATH 上已有 ssh（System32\OpenSSH）就跳过
```

`shim_dir = "bin"` 是自动发现版——`bin/` 下每个 `*.exe` 都自动 shim。
`cpp-toolchain` 给 llvm-mingw 的 ~270 个 binary 用这条（手写 `shims`
跟不上 upstream 版本漂移）。

#### 3. 安装后要跑脚本——`post_install`

```toml
[tool.vcpkg]
source = "github:microsoft/vcpkg"      # 无 release 资产，走 source-zip fallback
bin = "vcpkg.exe"
post_install = "bootstrap-vcpkg.bat"   # 路径相对 extracted artifact
```

post_install 在新解压时跑一次（cwd = artifact 根，Windows 走 `cmd /c`）。
vcpkg 自带 bootstrap 脚本所以直接能用。**当 upstream zip 只装载荷不带
install logic**（字体就是这种），用 `bp:` 前缀：

```toml
[tool.maple-mono]
source = "github:subframe7536/maple-font"
no_shim = true                          # 不是 CLI binary，没 PATH 入口
post_install = "bp:scripts/register-fonts.ps1"
# bp:<rel> 相对你这个 bp source repo 的根，不是 artifact 根。
# 脚本住你 bp 仓的 scripts/，不是 upstream zip 里。
```

实际 bp：`Coh1e/luban-bps/blueprints/fonts.toml`。把 artifact 里所有
`.ttf` 注册到 HKCU\…\Fonts + AddFontResourceEx（per-user，零 UAC）。

#### 4. 给工具配 dotfile——`config` 块

```toml
[config.git]                            # 用内置 git renderer
userName = "alice"
userEmail = "alice@example.com"
lfs = true                              # 加 [filter "lfs"] 块
[config.git.aliases]
co = "checkout"
br = "branch"
```

内置 renderer：`git` / `bat` / `fastfetch` / `yazi` / `delta`。输出走
drop-in `~/.gitconfig.d/<bp>.gitconfig`；用户主 `~/.gitconfig` 只需
`[include]` 一次该目录。**自定义 renderer 走 Lua 蓝图**是 v0.3.0 工作
（议题 M(d)）。

#### 5. 直接落一个文件（4 种 mode）

```toml
# (a) replace：整覆盖目标，首次 apply 会备份原文件
[file."~/.config/starship.toml"]
mode = "replace"
content = '''
add_newline = false
[character]
success_symbol = "[❯](bold green)"
'''

# (b) drop-in：写到 <canonical>.d/<bp>，跟用户主文件并列
[file."~/.gitconfig.d/work-aliases.gitconfig"]
mode = "drop-in"
content = "[alias]\n    co = checkout\n"

# (c) merge：JSON Merge Patch (RFC 7396)——只动你列出的 key，其他保留。
#     用例：WT settings.json 的 themes 部分，不动其它字段。
[file."~/AppData/Local/Packages/Microsoft.WindowsTerminal_8wekyb3d8bbwe/LocalState/settings.json"]
mode = "merge"
content = '''
{
  "profiles": {
    "defaults": {
      "font": { "face": "Maple Mono NF CN", "size": 11 }
    }
  }
}
'''

# (d) append：把 content 包在 luban marker block 里（按 bp 名 key），
#     重 apply 替换块内容不重复堆，幂等。多 bp 共享 profile.ps1 不打架。
[file."~/Documents/PowerShell/Microsoft.PowerShell_profile.ps1"]
mode = "append"
content = '''
Invoke-Expression (& { (zoxide init powershell | Out-String) })
Invoke-Expression (&starship init powershell)
'''
```

#### 6. 叠层——`meta.requires`

```toml
[meta]
requires = ["main/foundation", "main/cli-tools"]   # apply 时强制检查
conflicts = ["main/legacy-cpp-base"]               # 互斥（计划中）
```

当前 generation 没装 `main/foundation` 时 `bp apply` 直接 fail，错误信息
里给出**精确的** `luban bp apply main/foundation` 命令复制粘贴。
install.ps1 两阶段 bootstrap（foundation 无 prompt 自动装、cpp-toolchain
prompt）让新机器从一开始就满足 gate。

#### 7. 个人 onboarding bp 模板

一台机器一次到位的常见 shape：

```toml
schema = 1
name = "onboarding"
description = "Win11 个人配置"

# 装基础 3 件没覆盖的工具
[tool.gh]
source = "github:cli/cli"
[tool.lazygit]
source = "github:jesseduffield/lazygit"

# 配置 foundation 已装的工具
[config.git]
userName = "Coh1e"
userEmail = "you@example.com"
[config.git.credential]
helper = "manager"

# 落 dotfiles
[file."~/.config/starship.toml"]
mode = "replace"
content = '''...'''

[file."~/Documents/PowerShell/Microsoft.PowerShell_profile.ps1"]
mode = "append"
content = '''
Set-Alias ll Get-ChildItem -Force
Invoke-Expression (&starship init powershell)
'''

[meta]
requires = ["main/foundation", "main/cli-tools", "main/fonts"]
```

存到你自己的 bp source repo（如 `~/dotfiles-bp/blueprints/onboarding.toml`），
注册 + 应用：

```pwsh
luban bp src add D:\dotfiles-bp --name me
luban bp apply me/onboarding
```

新机器流程：`irm install.ps1 | iex` → installer 自动 apply foundation +
prompt cpp-toolchain → `luban bp apply main/cli-tools main/fonts me/onboarding`
就回来了。网络给力的话 ~3 分钟。

#### 8. PowerShell 模块 — `pwsh-module:` source scheme（v0.4.1+）

只发 PowerShell 模块的工具（PSReadLine / PSFzf / posh-git 等）有专门
scheme。.nupkg 本身就是 zip，luban 现有的 extract 直接处理。配套
`post_install` 脚本把模块文件拷到 `~/Documents/PowerShell/Modules/<Name>/<Version>/`
让 pwsh `$PSModulePath` 自动发现。

```toml
[tool.psreadline]
source = "pwsh-module:PSReadLine"
version = "2.4.0"           # 必填（auto-latest 是 v0.4.x 后续）
no_shim = true              # 模块走 PSModulePath 不走 PATH
bin = "PSReadLine.psd1"
post_install = "bp:scripts/install-pwsh-module.ps1"
```

实际例子：`Coh1e/luban-bps/blueprints/pwsh-modules.toml` +
`scripts/install-pwsh-module.ps1`。脚本通用——从 .psd1 manifest 读
名字 + 版本，同一脚本处理所有模块。Per-user，零 UAC。

#### 9. Lua bp — `register_renderer` / `register_resolver`（v0.4.0 / v0.4.2）

需要内置 5 个之外的 config renderer（`git`/`bat`/`fastfetch`/`yazi`/`delta`），
或 `github:` / `pwsh-module:` 之外的 source scheme，写 Lua bp。Tier 1
（DESIGN §9.9）让 bp inline 注册两者：

```lua
-- onboarding.lua

-- 注册 [config.starship] 的自定义 renderer
luban.register_renderer("starship", {
  target_path = function(cfg, ctx)
    return ctx.xdg_config .. "/starship.toml"
  end,
  render = function(cfg, ctx)
    return string.format(
      "add_newline = %s\ncommand_timeout = %d\n",
      tostring(cfg.add_newline or false),
      cfg.command_timeout or 1000)
  end,
})

-- 注册 `emsdk:` source scheme（emscripten 在 Google Storage，不是 GitHub）
luban.register_resolver("emsdk", function(spec)
  return {
    url = string.format(
      "https://storage.googleapis.com/webassembly/emscripten-releases-builds/win/%s/wasm-binaries.zip",
      spec.version),
    sha256 = "sha256:<one-time-pin>",
    bin = "emscripten/emcc.bat",
  }
end)

return {
  schema = 1,
  name = "onboarding",
  tool = {
    emscripten = { source = "emsdk", version = "1724b50443d92e23ef2a56abf0dc501206839cef" },
  },
  config = {
    starship = { add_newline = false, command_timeout = 500 },  -- → 上面那个 renderer
  },
}
```

Apply 时：
1. luban 解析 bp **两次** — 第一次拿 spec，第二次在长寿命 engine 里跑 `register_*` 副作用（refs 落到两个 registry）
2. Lock 解析**先**查 resolver registry（`emsdk:` 命中 bp 的 Lua fn，不 fall through 报"未知 scheme"）
3. Render 阶段**先**查 renderer registry（`[config.starship]` 命中 bp 的 fn，不走 builtin）

两个 registry 都是 per-apply 范围，bp 之间不串。TOML bp 不能 `register_*`
（没 Lua），但**能**引用前一个 Lua bp 注册过的 scheme / renderer
（DESIGN §9.9 "同一注册表 / 无双码路径"）。

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
