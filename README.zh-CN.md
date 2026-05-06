# Luban (鲁班)

[![license](https://img.shields.io/badge/license-MIT-blue)](LICENSE)
[![English](https://img.shields.io/badge/English-README-lightgrey)](README.md)

**给一张图纸，搭一座 C++ 工坊。**

luban 读一份 Lua / TOML 图纸（blueprint），照图纸把这台机器的 C++ 工作面装齐：
工具链（LLVM-MinGW + cmake + ninja + mingit + vcpkg）、CLI 利器（ripgrep / fd /
bat / ...）、配置文件（git / bat / fastfetch / ...）。多张图纸像 layer 一样叠，
可原子 rollback。

单一静态链接二进制（~5 MB），零 UAC，XDG-first，内嵌 Lua 5.4。Windows-first；
POSIX 可构建。

```bat
:: 1. 搭工坊（工具链 + CLI 利器）
luban bp apply embedded:cpp-base
luban bp apply embedded:cli-quality
luban env --user

:: 2. 创建 + 构建项目
luban new app hello && cd hello
luban add fmt && luban build
```

## 它是什么

C++ 在 Windows 上原本要装的散件（toolchain / cmake / ninja / clangd / vcpkg /
git + 一堆 cmake 胶水代码）合在一起几天的活，luban 把它做成 **两层叠图纸**：

- **承重墙**：`embedded:cpp-base` 装编译器 / cmake / vcpkg / 等
- **工作台**：`embedded:cli-quality` 装日常 CLI 利器 + dotfiles
- **装修**：用户写自己的 `~/.config/luban/blueprints/dev.lua` 加私有工具与配置

之后每个项目里：

```bat
luban new app foo                    :: 脚手架 + 自动 build，clangd 立即可用
cd foo
luban add fmt                        :: 写 vcpkg.json + 重渲 luban.cmake
luban build                          :: cmake/vcpkg manifest 自动取依赖
build\default\foo.exe                :: hello from foo!
```

**用户全程不打开 `CMakeLists.txt`、不写 `find_package`、不读 vcpkg manifest mode 文档。**

## 它不是什么

- **不是构建系统**——cmake + ninja 仍然负责构建
- **不是包管理器**——vcpkg 仍然负责 C++ 包解析与构建
- **不是替代品**——luban-managed 项目仍然是**标准 cmake 项目**，clone 到没装
  luban 的机器上 `cmake --preset default && cmake --build --preset default` 照样能 build

luban 是**带主见的胶水**——把对的工具用对的方式粘起来，给初学者**默认就走在轨道上**的体验。

## 命令清单（29 入口，5 组）

| 组 | 命令 |
|---|---|
| blueprint | `bp apply` `bp unapply` `bp ls` `bp status` `bp rollback` `bp gc` |
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

## 安装

下载 [最新 release](https://github.com/Coh1e/luban/releases/latest)
的 `luban.exe` 与 `luban-shim.exe`：

```bat
curl -L -o luban.exe https://github.com/Coh1e/luban/releases/download/<ver>/luban.exe
curl -L -o luban-shim.exe https://github.com/Coh1e/luban/releases/download/<ver>/luban-shim.exe

luban bp apply embedded:cpp-base
luban env --user
:: 关掉当前终端，开新终端，cmake / clang / clangd 直接可用
```

更新到新版：`luban self update`
完全卸载：`luban self uninstall --yes`（破坏性，不带 `--yes` 只打印计划）

## License

luban 自身：[MIT](LICENSE)。Vendored 第三方：
- `third_party/json.hpp` — nlohmann/json，MIT
- `third_party/miniz.{h,c}` — Rich Geldreich，BSD-3-Clause（`third_party/miniz.LICENSE`）
- `third_party/toml.hpp` — Mark Gillard 的 toml++，MIT（`third_party/toml.LICENSE`）
- `third_party/doctest.h` — Viktor Kirilov，MIT（`third_party/doctest.LICENSE`）
- `third_party/lua54/` — Lua 5.4，MIT（`third_party/lua54/LICENSE`）
