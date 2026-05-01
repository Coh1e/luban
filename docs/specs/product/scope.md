# 范围（Scope）

明确 "luban 管什么、不管什么" 的边界。任何超出本页范围的功能请求需先开 ADR。

## 在范围内（In scope）

| 主题 | 具体边界 |
|---|---|
| 工具链分发 | LLVM-MinGW、cmake、ninja、mingit、vcpkg、emscripten、node（emsc 依赖）|
| 工程脚手架 | `luban new app/lib`，含原生 + WASM 两种 target |
| 依赖编辑 | 通过 `luban add/remove/sync` 编辑 `vcpkg.json`，并刷新 `luban.cmake` |
| 构建透传 | `luban build` 选 preset、注入 PATH、运行 cmake/ninja，必要时 emcmake 包裹 |
| 环境集成 | `luban env --user` 写 HKCU 的 PATH + `LUBAN_*` + `VCPKG_ROOT` |
| Shim 体系 | text shims（cmd/ps1/sh）+ 硬链接的 `luban-shim.exe` rustup-style |
| 自管理 | `luban self update / uninstall` |
| 诊断 | `luban doctor / which / describe` |

## 不在范围内（Out of scope）

- **新建 build 系统、IR、DSL**——cmake 仍然是主体
- **替换 vcpkg manifest 格式**——`luban add` 直接编辑 `vcpkg.json`
- **运行时包管理**（pipx/npm-style 全局 CLI 工具）——v1 不做
- **多用户 / 系统级安装**——HKCU only
- **二进制中心仓库**——依赖 vcpkg 自身的二进制缓存
- **官方 IDE 插件**——生成 `compile_commands.json` 后由 editor/LSP 消费
- **跨平台 v1**——Linux/macOS 是 M4+

## 灰色地带（按情况开 ADR）

- 是否承担 **MSVC 工具链分发**（M4+ 候选；目前只支持 LLVM-MinGW）
- 是否引入 **`luban tool install`**（pipx 等价物）
- 是否做 **per-project toolchain pin**（rustup-toolchain 风格）
