# AGENTS.md

> 给 AI agent（Claude Code、其他 coding agent）的快速指引。比 `CLAUDE.md` 更面
> 向 "协作姿势"。CLAUDE.md 已经覆盖项目背景与命令清单，本文只补 "协作守则"。

> **代码必须有详细注释**——见 [`docs/specs/decisions/ADR-0002`](./docs/specs/decisions/ADR-0002-layered-strategy.md) 注释规范段。Agent 输出代码不得省略注释。

## 快速定位

- 项目背景、构建命令、模块概要 → 见 [`CLAUDE.md`](./CLAUDE.md)
- 设计/治理文档（产品、架构、ADR、模块、规划）→ 见 [`docs/specs/`](./docs/specs/)
- 用户文档（mdBook 源）→ 见 [`docs/src/`](./docs/src/)（已部署到 luban.coh1e.com）

## 协作守则

1. **小 PR、按职责拆分**：一次只解决一类问题。CLAUDE.md 的 "recipes" 区每条对应一
   个独立 PR 模板。
2. **不要 canonicalize 路径**：`D:\dobby` 等 junction 会破坏 `relative_to`。直接
   用用户输入的字面路径。
3. **luban.exe 自身不引第三方依赖**：除非是 single-header 且 vendor 到
   `third_party/` + LICENSE。**不嵌 Rust crate**（保 3MB 单 exe + 0 运行时依赖；
   见 ADR-0002 scope (1)）。luban-managed packages（OQ-7 luban registry）允许
   FFI 包 Rust 库——那是另一层 scope。
4. **`luban.cmake` schema 改动等于破坏性升级**：必须开 ADR + v2 标记 + 兼容读回。
5. **静态链接**：`-static -static-libgcc -static-libstdc++` 不能丢。
6. **HKCU only**：测试代码也不能尝试写 `HKLM`。
7. **代码注释质量**：注释解释 WHY 与上下文（不只是 WHAT）；公开 API 都有 doxygen
   风格 brief；复杂分支/状态机/边界条件必须 inline 注释；删代码同步删注释。
   反例 `// Increment x` 这种是不接受的。详见 ADR-0002 注释规范。

## 代码注释 quality（agent 输出代码必读）

luban 的代码注释政策是**严格的详细注释**——agent 不得省略，PR review 把"注释是否
解释了 WHY"作为合格标准。

- **每个 .cpp 顶部**有一段块注释，说明模块对外承诺什么 + 为什么是现在这个形态。
- **每个公共函数 / 类 / 结构**：一行 doxygen 风格 brief；必要时 `@param` /
  `@return` 描述非显然的语义。
- **复杂分支 / 状态机 / 边界条件 / workaround** 必须 inline 注释 WHY。
- **删代码同步删注释**——防 stale。

详见 [`docs/specs/decisions/ADR-0002-layered-strategy.md`](./docs/specs/decisions/ADR-0002-layered-strategy.md) 注释规范段。

## 常用 Agent 任务模板

### 加新 verb `foo`
1. `src/commands/foo.cpp`（含 `run_foo` + `register_foo`）
2. `CMakeLists.txt` 把 cpp 加到 `LUBAN_SOURCES`
3. `src/main.cpp` 增 `void register_foo();` + 调用
4. mdBook：`docs/src/commands/foo.md` + `docs/src/SUMMARY.md` + `docs/src/commands/overview.md`

### 加 vcpkg port → cmake target 映射
- 编辑 `src/lib_targets.cpp` 表，单条 `{port, find_package, {targets...}}`

### 加可 bootstrap 组件
1. `manifests_seed/<name>.json`：`version` `url` `hash` `extract_dir` `bin` `depends`
2. 启用：`manifests_seed/selection.json`
3. 若需 post-extract bootstrap：在 `src/component.cpp` 按 name 特例化（vcpkg、emscripten 已存在样板）

## 何时**不要**直接动手而是先开 ADR

- 改 `luban.cmake` v1 schema 任何字段
- 改 `installed.json` schema
- 改 verb 名 / 删 verb
- 加新顶层目录或第三方依赖
- 跨平台移植决策

## 期望的输出风格

- 匹配用户语言（用户讲中文就回中文，讲英文就回英文）
- 终端文字简洁，不要罗列冗余进度
- 提交信息使用项目最近的风格（见 `git log --oneline`），动词在前，无 emoji
- 输出代码必须配详细注释（ADR-0002 注释规范）
