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
5. **静态链接**（议题 R, dual-flavor）：MinGW 走 `-static -static-libgcc -static-libstdc++`，MSVC 走 `CMAKE_MSVC_RUNTIME_LIBRARY = MultiThreaded` (`/MT`)。CI 用 `dumpbin` / `llvm-readobj` 校验无 `vcruntime / msvcp / libgcc_s / libstdc++` 依赖；release 同时产 `luban-msvc.exe` + `luban-mingw.exe`，两条路径都不能退化到动态 CRT。
6. **HKCU only**：测试代码也不能尝试写 `HKLM`。
7. **shim 路径 = `~/.local/bin/`**：toolchain bin 绝不进 PATH（议题 G + 不变量 5）。任何代码写老 `<data>/bin/` 都是 v0.x 残留。
8. **代码注释质量**：注释解释 WHY 与上下文（不只是 WHAT）；公开 API 都有 doxygen 风格 brief；复杂分支/状态机/边界条件必须 inline 注释；删代码同步删注释。
9. **先量化再改代码**（DESIGN §25.8）：网络 / 性能 / 行为差异类 bug 第一动作是 `curl` / `gh api` / `dumpbin` / `Measure-Object` 等单行 probe 拿到数字，而不是直接 patch。无数据的猜测会让你修错根因——v0.1.0 修 rename 不修空 `artifact_id` 是这条原则被违反的代价。
10. **修根因不修症状**：错误冒出的位置往往不是错误的来源。看到莫名其妙的 PATH_NOT_FOUND / SHA mismatch / 速度异常时，向上游追到第一个产生病态值的边界——通常在 resolver / parser / config 写入处。下游 try-catch 是硬技术债。
11. **boundary guard 优于全链路容错**：在 trust boundary（用户输入、外部 API 返回、跨进程 IPC）显式 reject 非法值并报路径，比在十处下游各加 `if (empty) return early` 干净得多。`store::fetch` 拒绝空 `artifact_id` 是范式案例。

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
