# 对外接口

luban 的 "对外接口" 包含三个面，**任何破坏性变更需要 ADR**：

1. **CLI verbs**（最稳定）
2. **文件契约**（次稳定）
3. **环境变量约定**（次稳定）

## 1. CLI verbs（16 个 / 4 组）

| 组 | verb | 源文件 | 说明 | 是否 forward_rest |
|---|---|---|---|---|
| setup | `env` | `commands/env.cpp` | HKCU 注册 (`--user`) / 解除 / `--print` (eval) / `--msvc-init` (vcvarsall capture) | no |
| setup | `setup` | `commands/setup.cpp` | 按 selection.json 安装组件 | no |
| project | `new` | `commands/new_project.cpp` | 脚手架（含 wasm-app 变种） | no |
| project | `build` | `commands/build_project.cpp` | 自动 preset + cmake/ninja（必要时 emcmake） | no |
| project | `target` | `commands/target_cmd.cpp` | 增删 target | no |
| project | `doc` | `commands/doc.cpp` | doxygen 包装（cargo doc 风格） | no |
| dep | `add` | `commands/add.cpp` | 加 vcpkg 库 + 重渲染 luban.cmake | no |
| dep | `remove` | `commands/add.cpp` | 减 vcpkg 库 + 重渲染 | no |
| dep | `sync` | `commands/add.cpp` | 仅重渲染 luban.cmake | no |
| dep | `search` | `commands/which_search.cpp` | wraps `vcpkg search` | no |
| advanced | `doctor` | `commands/doctor.cpp` | 健康诊断（`--strict` exit code / `--json` schema=1） | no |
| advanced | `which` | `commands/which_search.cpp` | 解析 alias → 绝对 exe | no |
| advanced | `run` | `commands/which_search.cpp` | 注入 env 后透明 exec | **yes** |
| advanced | `shim` | `commands/shim_cmd.cpp` | 重生成 `<data>/bin/` 全部 shim（`--force` 覆盖碰撞） | no |
| advanced | `describe` | `commands/describe.cpp` | 系统 + 项目状态（`--json` / `--host` 模式） | no |
| advanced | `self` | `commands/self_cmd.cpp` | update / uninstall | no |
| advanced | `completion` | `commands/completion.cpp` | shell 补全脚本（当前：clink） | no |

## 2. 文件契约

| 文件 | 写者 | 读者 | schema 版本 | 兼容策略 |
|---|---|---|---|---|
| `<state>/installed.json` | `component::install` | luban 各模块 | `1` | 字段只增不减；新字段 optional |
| `<state>/msvc-env.json` (v0.2+) | `luban env --msvc-init` | `env_snapshot::env_dict()` | `1` | vcvarsall capture 的快照；改 schema 需读回兼容 |
| `<project>/luban.cmake` | `luban_cmake_gen` | cmake | v1（隐式） | 升级需保留旧 marker，可被旧 luban 读取 |
| `<project>/vcpkg.json` | luban + 用户 | vcpkg | vcpkg 自家 | 透传，不发明字段；可加 `luban_mirrors` 扩展（manifest 用） |
| `<project>/luban.toml` | 用户 | luban | v1 `[project]` `[scaffold]` | 新增段 optional，不破坏老项目；`cpp` ∈ {17,20,23} |
| `<config>/selection.json` | 用户 / setup | setup | `1` | components / extras 二分 |
| `<config>/emscripten/config` (v0.2+) | `component::install` (emscripten 特例) | emcc (via `EM_CONFIG`) | n/a | python-style，重装 emscripten 时重写 |
| `<data>/bin/.shim-table.json` | `luban shim` | `luban-shim.exe` | flat map | 同步重写 |

## 3. 环境变量约定

luban 写入 / 读取的环境变量：

| 变量 | 谁读 | 谁写 | 用途 |
|---|---|---|---|
| `LUBAN_PREFIX` | `paths.cpp` | 用户（容器/CI） | 把 4 个 home 都重定向到 `<prefix>/<role>` |
| `XDG_DATA_HOME` 等 | `paths.cpp` | 用户 | XDG-first 解析 |
| `XDG_BIN_HOME` | （目前未消费——保留） | 用户 | uv 风格扩展；luban 默认 bin 在 `<data>/bin` 不读它 |
| `VCPKG_ROOT` | vcpkg / cmake preset | `luban env --user` + `env_snapshot` | 指向已装 vcpkg |
| `VCPKG_DOWNLOADS` `VCPKG_DEFAULT_BINARY_CACHE` `X_VCPKG_REGISTRIES_CACHE` | vcpkg | `env_snapshot` | vcpkg cache 走 XDG（`<cache>/vcpkg/{downloads,archives,registries}`）|
| `EM_CONFIG` | emcc | `luban env --user` (when emscripten installed) + `env_snapshot` | 指向 `<config>/emscripten/config` |
| `INCLUDE / LIB / LIBPATH / WindowsSdk* / VCToolsInstallDir / 等 ~30 个` | cl / link / msbuild | `env_snapshot` (only when `<state>/msvc-env.json` exists) | MSVC vcvarsall 的 capture 结果，注入子进程 |
| `PATH` | 全部 | `env_snapshot` / `win_path` | prepend `<data>/bin` 与 toolchain bin 目录（+ MSVC path addition 当 capture 过） |
| **删除（v0.1.x 历史）**：`LUBAN_DATA` `LUBAN_CACHE` `LUBAN_STATE` `LUBAN_CONFIG` | — | — | 无消费者，v0.2 移除；`--unset-user` 仍清理老安装 |

## --json 约定（草案）

`luban describe --json`、`luban which --json` 等输出**稳定的机器可读结构**。建议
schema：

```json
{
  "schema": 1,
  "luban": { "version": "..." },
  "system": { "data_dir": "...", "components": [ "..." ] },
  "project": { "root": "...", "targets": [], "deps": [] }
}
```

新字段只能加，不能改名/删除（破坏需 ADR）。
