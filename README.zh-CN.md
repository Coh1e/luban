# Luban (鲁班)

[![docs](https://img.shields.io/badge/docs-luban.coh1e.com-blue)](https://luban.coh1e.com/)
[![api](https://img.shields.io/badge/api-luban.coh1e.com%2Fapi-green)](https://luban.coh1e.com/api/)
[![license](https://img.shields.io/badge/license-MIT-blue)](LICENSE)
[![English](https://img.shields.io/badge/English-README-lightgrey)](README.md)

Windows-first 的 C++ 工具链管理器 + cmake/vcpkg 辅助前端。
**单一静态链接二进制，零 UAC，XDG-first 目录布局。**

```bat
luban setup && luban env --user
luban new app hello && cd hello
luban add fmt && luban build
```

## 文档

- **用户手册** → [luban.coh1e.com](https://luban.coh1e.com/)（mdBook，叙事风）
- **贡献者参考** → [luban.coh1e.com/api](https://luban.coh1e.com/api/)（Doxygen）
- **源码** → 本仓库

## 它是什么

C++ 在 Windows 上原本要装的散件（toolchain / cmake / ninja / clangd / vcpkg / git
+ 一堆 cmake 胶水代码）合在一起几天的活，luban 把它做成了 **两条一次性命令**：

```bat
luban setup            :: 装 LLVM-MinGW + cmake + ninja + mingit + vcpkg（约 3 分钟，~250 MB）
luban env --user       :: rustup 风格 HKCU PATH 注入（一次性）
```

之后每个项目里只要：

```bat
luban new app foo                    :: 脚手架 + 自动 build，clangd 立即可用
cd foo
luban add fmt                        :: 编辑 vcpkg.json + 重生成 luban.cmake
luban build                          :: cmake 通过 vcpkg manifest mode 自动取依赖
build\default\src\foo\foo.exe        :: hello from foo!
```

**用户全程不打开 `CMakeLists.txt`，不写 `find_package`，不读 vcpkg manifest mode 文档。**

## 它不是什么

- **不是构建系统**——cmake + ninja 仍然负责构建
- **不是包管理器**——vcpkg 仍然负责 C++ 包解析与构建
- **不是替代品**——luban-managed 项目仍然是**标准 cmake 项目**，clone 到没装
  luban 的机器上 `cmake --preset default && cmake --build --preset default`
  照样能 build

luban 是**带主见的胶水**——把对的工具用对的方式粘起来，给初学者**默认就走在轨道上**的体验。

## 命令清单（16 个 verb，4 组）

| 组 | 命令 |
|---|---|
| 工具链 / 环境 | `setup` `env` |
| 单项目 | `new` `build` `target` |
| 依赖管理 | `add` `remove` `sync` `search` |
| 诊断 / 自治 | `doctor` `run` `which` `describe` `shim` `self` |

完整说明见 [the Luban Book](https://luban.coh1e.com/)。

## 设计哲学（一行版）

cmake 永远是项目主角；luban 是**辅助**。luban 写 `luban.cmake`（一个标准 cmake module），
你的 `CMakeLists.txt` `include()` 即用——一行 luban 痕迹。删掉那行 luban 就消失，
项目变回普通 cmake 工程。

完整 8 条架构不变量见 [Design summary](https://luban.coh1e.com/architecture/design.html)。

## 安装

下载 [最新 release](https://github.com/Coh1e/luban/releases/latest)
的 `luban.exe` 与 `luban-shim.exe`：

```bat
curl -L -o luban.exe https://github.com/Coh1e/luban/releases/download/v0.1.2/luban.exe
curl -L -o luban-shim.exe https://github.com/Coh1e/luban/releases/download/v0.1.2/luban-shim.exe

luban setup
luban env --user
:: 此时关掉当前终端，开新终端，cmake / clang / clangd 直接可用
```

更新到新版：`luban self update`
完全卸载：`luban self uninstall --yes`（破坏性，不带 `--yes` 只打印计划）

## License

luban 自身：[MIT](LICENSE)。Vendored 第三方：
- `third_party/json.hpp` — nlohmann/json，MIT
- `third_party/miniz.{h,c}` — Rich Geldreich，BSD-3-Clause（`third_party/miniz.LICENSE`）
- `third_party/toml.hpp` — Mark Gillard 的 toml++，MIT（`third_party/toml.LICENSE`）
