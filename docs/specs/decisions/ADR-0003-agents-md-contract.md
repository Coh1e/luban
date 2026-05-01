# ADR-0003 — `AGENTS.md` 是 luban → AI 的总传话机；SAGE 是其中一段

- **状态**：Accepted (v0.2)
- **日期**：2026-05-01
- **作者**：Coh1e + Claude
- **取代**：n/a

## Context

用户希望 luban 提供 "AI 编程辅助能力"。这容易被误解为"luban 内嵌 AI 客户端 / 调
LLM API"——这条路 luban 已经决定不走（不引第三方运行时依赖；也不打算和具体 AI 厂
商绑定）。

实际有用的能力是反过来的：**luban 已经知道很多关于这个项目的事**——cpp 标准、
toolchain 版本、sanitizer 配置、deps、targets——把这些**结构化地、显式地交给 AI
agent**，让 agent 给的建议从 "blanket modern C++ tip" 升级到 "以这个项目的真实
配置为前提的具体建议"。

工业界事实标准：项目根放一份 `AGENTS.md` / `CLAUDE.md`，agent 启动时读。luban
的角色是**生成 + 维护这份文件**，不是替 agent 思考。

同时用户提了 SAGE（Scenario-first AI Guided Elicitation）方法：让 AI 引导真实场景
对话产出结构化需求文档。这是一个**对话流程**，不是 luban 自己跑的——但 luban 可以
脚手架对应的目录结构 + 在 AGENTS.md 里把"流程怎么走"教给 agent。

两件事可以共用一份 `AGENTS.md`：项目体检报告 (luban 在每次 sync 时刷新) +
SAGE workflow guide (静态文本) + 用户自由编辑区。

## Decision

`AGENTS.md` 是 luban → AI 的**唯一总传话机**。其余文件（`CLAUDE.md` /
`specs/` 目录骨架）都是它的辅助：

- **`CLAUDE.md`**：一行 indirection (`# This project's AI guidance lives in
  [AGENTS.md].`)。给 hardcode 了 `CLAUDE.md` 名字的 agent 用。**不是真 symlink**
  （Windows 上需 admin / dev mode），用文本 indirection。
- **`specs/`**：建议结构（README、HOW-TO-USE、`sage/<topic>/` 子目录骨架）。
  内容由 user 和 agent 协作产出；luban 只创建空骨架。
- **AGENTS.md 本身**：5 段，前 3 段 luban-managed，第 4 段 SAGE 流程静态文本，
  第 5 段用户自由。

### marker block 协议

```
<!-- BEGIN luban-managed: <section-name> -->
... content ...
<!-- END luban-managed -->
```

- `section-name` 是 kebab-case，目前定义 3 个：`project-context` /
  `cpp-modernization` / `ub-perf-guidance`。
- `luban specs sync` 解析这些 marker，**只重写 marker 之间的内容**；marker
  外面 100% 保留。
- 用户可以**整段删除 marker**——luban 检测到 marker 缺失就 skip 那段（"用户
  opted out"）。不会再自动加回。
- BEGIN 必须有匹配 END；不匹配则 sync 跳过那段并 warn。
- 不支持嵌套 marker。
- 同一 `section-name` 出现两次以上是 user 写错了；luban 只处理第一次出现。

### luban-managed 段的内容

| 段 | 来源 | 何时变化 |
|---|---|---|
| `project-context` | `luban.toml [project]` cpp/triplet + `vcpkg.json dependencies` + `CMakeLists.txt add_executable/library` 名字 + `<state>/installed.json` 工具链版本 + `luban.toml [scaffold] sanitizers` | 任何这些源变化时 user 跑 `luban specs sync` |
| `cpp-modernization` | 静态模板（C++23/20 idiom 偏好） | 模板更新（很少） |
| `ub-perf-guidance` | 静态模板 + 一个根据 sanitizers 配置变化的小段（`{{ub_section}}`） | sanitizer 配置变化或模板更新 |

第 4 段（SAGE workflow guide）和第 5 段（user notes）luban 永不触碰。

### `specs/` 是建议结构，不是规范

`specs/HOW-TO-USE.md` 在文件开头明示这一点。用户可以：

- 用单文件 (`01-onboarding.md`) 替代 `sage/<topic>/` 子目录
- 用别的方法学（STAR / 5W / Job Story）替代 SAGE
- 删 `specs/` 整个目录、只用 AGENTS.md
- 改 marker 之外的 SAGE workflow 段教 agent 走新流程

luban 不强求结构。**承诺**只有：marker block 内的内容是 luban 的；其余是用户的。

## 拒绝的替代方案

### A. 把项目环境写进单独的机器读结构（如 `<root>/.luban-state.json`）

- 优：不污染 markdown，agent 可 jq 查询。
- 劣：现实中 agent 的"读环境"路径就是读 `AGENTS.md`/`CLAUDE.md`。再加一份
  额外文件让 agent 找不到 / 用户不读。Markdown 是**人 + 机** 都能消费的格式，
  最低门槛胜出。

### B. luban 自己跑 LLM API 调用（"AI 客户端"）

- 优：用户 `luban ask "..."` 一条命令搞定。
- 劣：拖入 OpenAI/Anthropic SDK / API key 管理 / 与 ADR-0001 invariant 7
  （单 static binary 0 运行时依赖）冲突。且选错家会变成 vendor lock。

### C. 不用 marker block，整份 AGENTS.md 都 luban 管

- 优：实现简单。
- 劣：用户的项目特殊约定无处放；user 编辑会被 sync 抹掉。这条违反"用户拥有
  的部分必须可控"原则。

### D. 用真 symlink CLAUDE.md → AGENTS.md

- 优：物理上单一文件。
- 劣：Windows 默认禁 symlink（要 admin / dev mode）；CI runner 经常拒。文本
  indirection 兼容性更好，agent 也认。

## 后果

- **正面**：AGENTS.md 是单一交接点；luban-managed 段自动随项目状态刷新；用户
  对自己写的部分有完全主权；user 可以重构 specs/ 自由（建议非规范原则）。
- **负面**：marker block 是个微型 schema；用户看到注释 `<!-- BEGIN luban-managed ... -->`
  心理负担轻微非零。
- **维护负担**：增 / 改 luban-managed 段名字算 schema 变更——以后改 ADR。

## 验证

- `luban specs init` 在带 vcpkg.json + luban.toml + CMakeLists.txt 的项目里
  渲染出准确的 project-context 段（cpp / triplet / sanitizers / deps / targets
  全部正确）——v0.2 已验证。
- `luban specs sync` 改 luban.toml 的 cpp 后 refresh AGENTS.md，user 自由段
  保留——v0.2 已验证。
- `luban specs new <topic>` 创建 `specs/sage/<topic>/{scene,pain,mvp}.md`
  骨架——v0.2 已验证。
- `luban specs status` 给 SAGE 单元 fill 状态报告——v0.2 已验证（heuristic）。

## 跨页索引

- ADR-0001 — 8 条架构不变量（"AI 客户端不在 luban 内"的源头）
- ADR-0002 — luban.exe 自身不引第三方运行时依赖；luban-managed packages 另说
- `../../src/commands/specs.cpp` — 实现
- `../../templates/specs/` — 模板根
- `../../docs/src/commands/specs.md` — 用户手册章节（也是 `luban specs --help`
  的长 help 源 + `specs/HOW-TO-USE.md` 的内容源——三处同步）
