# 总体架构

> 本页是 `../../src/architecture/design.md` 的浓缩版（面向"团队"而非"用户"）。
> 详细说理见 mdBook 源 `../../src/architecture/{philosophy,two-tier-deps,why-cmake-module}.md`。

## 三层模型

```
┌─ 用户层 ────────────────────────────────────────────────────┐
│  luban CLI: 16 verbs / 4 groups (setup, project, dep, adv) │
└────────────────────────────────────────────────────────────┘
            │ 编辑 vcpkg.json / luban.toml
            │ 调用 cmake / vcpkg
            ▼
┌─ luban 中间层（"辅助胶水"）───────────────────────────────┐
│  luban.cmake          → 用户项目里 git-tracked            │
│  <data>/bin/          → shim 目录，写入用户 PATH          │
│  <state>/installed.json → 工具链 registry                 │
│  selection.json / *.manifest → 系统级 manifest           │
└────────────────────────────────────────────────────────────┘
            │ 通过 PATH + env_snapshot 调用
            ▼
┌─ 外部工具 ─────────────────────────────────────────────────┐
│  cmake / ninja / clang / clangd / vcpkg / git              │
│  （luban 下载并管理，但配置文件保持原样）                    │
└────────────────────────────────────────────────────────────┘
```

luban 是中间层。删去中间层、保留下载下来的工具，仍是一套可工作的 cmake 项目。

## 8 条架构不变量（破坏即违约）

1. cmake 仍是主体（不发明 IR、不替换 manifest）
2. `luban.cmake` git-tracked、是标准 cmake 模块
3. 不发明新 manifest 格式（`vcpkg.json` 即唯一项目依赖来源）
4. `luban.toml` 可选，仅承载项目级偏好（warnings/sanitizer/preset/triplet）
5. XDG-first 路径，即使 Windows 上也是
6. 零 UAC，HKCU only
7. 单文件 static-linked 二进制
8. 两层依赖（系统 toolchain vs 项目 lib）严格分离

## 关键运行流（速查）

| 流 | 入口 | 关键代码 |
|---|---|---|
| 初次安装 | `luban setup` | `commands/setup.cpp` → `expand_depends` → `component::install` |
| 注册 PATH | `luban env --user` | `commands/env.cpp` → `win_path::add_to_user_path` |
| 创建项目 | `luban new app foo` | `commands/new_project.cpp::materialize` 模板展开 |
| 添加库 | `luban add fmt` | `commands/add.cpp` → `vcpkg_manifest::add` → `luban_cmake_gen::regenerate_in_project` |
| 构建 | `luban build` | `commands/build_project.cpp` → 自动选 preset → `proc::run("cmake")`（必要时 emcmake 包裹） |
| 解析 alias | `luban which` / shim 自身 | `registry::resolve_alias` 读 `installed.json` |

## 跨页索引

- 模块边界 → `module-boundaries.md`
- 技术栈 → `technology-stack.md`
- 领域实体 → `../domain/domain-model.md`
- 生命周期事件 → `../domain/event-model.md`
- ADR-0001 → `../decisions/ADR-0001-initial-architecture.md`
