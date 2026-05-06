# AGENTS.md

> 给 AI agent（Claude Code、其他 coding agent）的协作守则。项目背景、架构、命令清单见
> [`CLAUDE.md`](./CLAUDE.md) + [`docs/DESIGN.md`](./docs/DESIGN.md)。本文只补"协作姿势"。

> **代码必须有详细注释**——见 [`docs/DESIGN.md`](./docs/DESIGN.md) §20 注释规范。Agent 输出代码不得省略注释。

## 快速定位

- 项目背景、构建命令、模块概要、recipes → [`CLAUDE.md`](./CLAUDE.md)
- 设计文档（架构、领域、动词、决议）→ [`docs/DESIGN.md`](./docs/DESIGN.md)
- 字母序决议表（A → AB）→ [`docs/DESIGN.md`](./docs/DESIGN.md) §24.1

## 协作守则

1. **小 PR、按职责拆分**：一次只解决一类问题。CLAUDE.md 的 "recipes" 区每条对应一个独立 PR 模板。
2. **不要 canonicalize 路径**：`D:\dobby` 等 junction 会破坏 `relative_to`。直接用用户输入的字面路径。
3. **luban.exe 自身不引第三方依赖**：除非是 single-header 且 vendor 到 `third_party/` + LICENSE。**不嵌 Rust crate / DLL**。Lua 5.4 是当前唯一多文件例外（QJS 是 v1.x 议题 T，本期不 vendor）。
4. **`luban.cmake` schema 改动 = 破坏性升级**：必须 v2 标记 + 兼容读回。
5. **静态链接**：`-static -static-libgcc -static-libstdc++` 不能丢。
6. **HKCU only**：测试代码也不能尝试写 `HKLM`。
7. **shim 路径 = `~/.local/bin/`**：toolchain bin 绝不进 PATH（议题 G + 不变量 5）。任何代码写老 `<data>/bin/` 都是 v0.x 残留。
8. **代码注释质量**：注释解释 WHY 与上下文（不只是 WHAT）；公开 API 都有 doxygen 风格 brief；复杂分支/状态机/边界条件必须 inline 注释；删代码同步删注释。

## 代码注释（agent 输出代码必读）

luban 的代码注释政策是**严格的详细注释**——agent 不得省略，PR review 把"注释是否解释了 WHY"作为合格标准。

- **每个 .cpp 顶部**：一段块注释，说明模块对外承诺什么 + 为什么是现在这个形态。
- **每个公共函数 / 类 / 结构**：一行 doxygen 风格 brief；必要时 `@param` / `@return` 描述非显然的语义。
- **复杂分支 / 状态机 / 边界条件 / workaround**：inline 注释 WHY。
- **删代码同步删注释**——防 stale。

详见 [`docs/DESIGN.md`](./docs/DESIGN.md) §20。

## 常见任务模板

详 [`CLAUDE.md`](./CLAUDE.md) "Common task recipes" 节。涵盖：
- 加新 CLI verb
- 加 vcpkg port → cmake target 映射
- 给内置 blueprint 加 tool / resolver / renderer
- 加新内置 config renderer
- Cut release

## 何时**不要**直接动手，先讨论设计

- 改 `luban.cmake` v1 schema 任何字段
- 改 generation / Lock JSON schema
- 改 verb 名 / 删 verb / 加新 verb
- 加新顶层目录或第三方依赖
- 跨平台移植决策（POSIX shim / install.sh 等）
- 触碰 §6 八不变量

## 期望的输出风格

- 匹配用户语言（用户讲中文就回中文，讲英文就回英文）
- 终端文字简洁，不要罗列冗余进度
- 提交信息使用项目最近的风格（见 `git log --oneline`），动词在前，无 emoji
- 输出代码必须配详细注释（§20 注释规范）
