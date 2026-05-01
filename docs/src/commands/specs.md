# `luban specs`

Scaffold a project's requirements / design surface so AI coding agents
(Claude Code, Cursor, etc.) can give project-specific advice instead of
generic boilerplate. This is **luban's "AI 编程辅助" plumbing**: it doesn't
call any AI itself — it builds the road signs the AI will read.

The directory `specs/` and the file `AGENTS.md` it scaffolds are a
**suggestion, not a spec**. Restructure freely.

```
luban specs init                   Scaffold specs/ + AGENTS.md + CLAUDE.md
luban specs sync                   Refresh AGENTS.md's luban-managed sections
luban specs new <topic>            Add a new SAGE unit: specs/sage/<topic>/
luban specs status                 List SAGE units and their completeness
```

## 是什么

SAGE = Scenario-first AI Guided Elicitation. 你用自然语言告诉 AI 一个真实
场景、痛点、目标，AI 把它逐步提炼成结构化的需求文档。luban 提供脚手架 —
目录骨架 + AGENTS.md 引导 — 让你跟 AI 的对话有据可依。

`luban specs init` 在项目根落两类东西：

1. **`AGENTS.md`** — 项目级文档，告诉 AI agent 这个项目的工程上下文 +
   SAGE 工作流。前 4 段由 luban 渲染（含 marker block，`luban specs sync`
   可重新渲染），最后一段你自己写。
2. **`specs/` 目录骨架** — 一个空的 `sage/` 目录 + 一份 `HOW-TO-USE.md`。
   这是建议结构。AI 边和你聊，边在 `specs/sage/<topic>/` 下建文件。

## 一图概览

```
<your-project>/
├── AGENTS.md                  ← 项目级指引，agent 必读
│   ┌─────────────────────────────────────────┐
│   │ <!-- BEGIN luban-managed: project-context --> │  ← `luban specs sync` 重渲染
│   │   C++23 / x64-mingw-static / sanitizers / deps / targets │
│   │ <!-- END luban-managed -->                    │
│   │ <!-- BEGIN luban-managed: cpp-modernization --> │
│   │   现代 C++ idiom 偏好                          │
│   │ <!-- END luban-managed -->                    │
│   │ <!-- BEGIN luban-managed: ub-perf-guidance --> │
│   │   UB / 性能检查指引                            │
│   │ <!-- END luban-managed -->                    │
│   │                                                │
│   │ ## SAGE workflow (静态文本)                    │
│   │ ## Project-specific notes (用户自由编辑)        │
│   └────────────────────────────────────────────────┘
│
├── CLAUDE.md                  ← 一行 → AGENTS.md（兼容只识 CLAUDE.md 的 agent）
│
└── specs/                     ← 建议结构，可重构
    ├── README.md              ← 说明这是建议非规范
    ├── HOW-TO-USE.md          ← `luban specs init` 时落，等同此文档
    └── sage/                  ← 各场景独立 SAGE 单元
        ├── onboarding/
        │   ├── scene.md       ← 场景描述（用户填）
        │   ├── pain.md        ← 痛点
        │   └── mvp.md         ← MVP 边界 + 用户故事 + 验收
        ├── bug-report/
        │   ├── scene.md
        │   ├── pain.md
        │   └── mvp.md
        └── …                  ← 攒到 2+ 后让 AI compose 到 specs/composed.md
```

## 文件分工（每文件一节）

### `AGENTS.md`

luban 写给 AI 看的**项目体检报告 + 工作流约定**。5 段：

1. `project-context` *(luban-managed)* — C++ 标准 / triplet / toolchain 版本 /
   sanitizers 配置 / vcpkg deps / cmake targets。从 `luban.toml` + `vcpkg.json` +
   `<state>/installed.json` 抓。
2. `cpp-modernization` *(luban-managed)* — 现代 C++ idiom 偏好（ranges /
   `std::format` / `std::expected` / RAII / 不用 C-style cast）。
3. `ub-perf-guidance` *(luban-managed)* — UBSan 提示 / 常见陷阱 / profiler
   推荐 / profileable build flags。
4. `SAGE workflow` *(静态)* — 引导 AI 怎么走 SAGE 流程：鼓励多 SAGE 单元、
   组合优先、specs/ 自由重构。
5. `Project-specific notes` *(用户自由编辑)* — luban 永不触碰。写你想让 AI
   知道的项目特殊约定。

`marker block` 协议：`<!-- BEGIN luban-managed: <name> -->` 到
`<!-- END luban-managed -->` 之间是 luban 拥有的；外面是你拥有的。`luban specs sync`
只重写 marker 内的，外面 100% 保留。**marker 你也可以删** — 删了 luban 不会
再 sync 那段，外面的内容都是你的。

### `CLAUDE.md`

一行：

```markdown
# This project's AI guidance lives in [AGENTS.md](./AGENTS.md).
```

只为兼容只看 `CLAUDE.md` 的 agent。不用真 symlink（Windows 上需 admin / dev
mode），用文本 indirection。AI 会读这行后跳过去看 AGENTS.md。

### `specs/sage/<topic>/scene.md`

**写什么**：用你自己的话讲一个真实场景。第一人称、无技术词、像跟同事吐槽。

**示例**：

> "周一早上，我打开三个旧项目想看哪个还能 build。第一个 cmake 报 vcpkg 路径不
> 对——我去年装的某 cmake 版本现在不喜欢这个 toolchain；第二个 clang-tidy 直接
> 把 IDE 卡死；第三个能 build 但跑出来段错。三件事都让我恶心。我希望一句
> `luban doctor` 就告诉我哪个项目能干、哪个要修。"

**踩坑**：

- 别写解决方案，只写场景（"我希望 luban 怎么样" OK，"luban 应该 add 一个 X verb"
  不 OK——后者偷跑去做 mvp.md 的工）。
- 别写技术词。"段错" "build 失败" "死锁" 是感受词，OK；"SIGSEGV at offset 0x40"
  是分析，留给 mvp.md。

### `specs/sage/<topic>/pain.md`

**写什么**：场景里**痛**在哪。为什么现在解决，而不是去年 / 明年。

**示例**：

> "三件事都是我自己装的工具的副作用，但我**记不清**当时为什么这么装。每次重装
> 工具链都丢配置，每次开新项目都重学一遍 vcpkg manifest 模式。痛点不是工具不
> 好用，是**信息逃逸** — 我自己的设置我自己不记得。"

**踩坑**：别只复述场景。pain 是 "scene + 为什么这是 bug 而不是世界本来如此"。

### `specs/sage/<topic>/mvp.md`

**写什么**：解掉痛、能交付的最小切片。含：

- **边界**（什么算 MVP，什么不算 — 显式排除）
- **用户故事**（1-3 条，"作为 X 我想 Y 以便 Z"）
- **验收示例**（具体输入 → 期望输出，给 AI 的对照样本）

**示例**：

```markdown
## MVP 边界
in:
  - 一句 `luban doctor` 输出"这个项目能 build / 不能 build / 缺啥"
  - 不能 build 时给具体修复命令
out:
  - 不自动修，让 user 决定（避免误改）

## 用户故事
1. 作为重新打开旧项目的 dev，我想 `cd <proj> && luban doctor` 5 秒内告诉我
   是否需要 `luban setup` / `luban env --user`，避免无声卡 30 分钟。

## 验收示例
$ cd ~/old-projects/foo
$ luban doctor
✗ vcpkg toolchain version mismatch (manifest expects 4.3.x, registry has 4.0.x)
  fix: luban setup --with cmake --force
✓ ...
```

### `specs/composed.md`（可选）

当 `sage/` 下攒到 2+ 单元后，让 AI "compose 一下" — 它会把跨场景的用户故事 /
验收 / MVP 边界聚合到这里。luban 不替你写这个文件；让 AI 边对话边产出。

## 跑 AI 怎么用这个目录

1. 项目根 `AGENTS.md` 已经把这个目录指给 AI——你不用做啥。
2. 跟 AI 讲你的新场景。AI 读完 AGENTS.md，会问：
   - 这是新 SAGE 单元还是已有单元的扩展？
   - 如果新：选个 topic 名，调 `luban specs new <topic>` 建骨架。
3. AI 引导你 scene → pain → mvp 三步走，每步问几个 prompt。
4. 攒到 2+ SAGE，让 AI "compose 一下"，它会写 `specs/composed.md`。
5. 项目代码写到一半你改了 deps / cpp 标准 / sanitizer 配置 — 跑 `luban specs sync`
   refresh AGENTS.md 的 luban-managed 段。

## 重构原则（你能改这个目录吗）

能。**你产出的"结构化原始需求"才是核心**，目录形态是道具。

- 想用 `01-onboarding.md` `02-bug.md` 单文件而非 `sage/<topic>/` 子目录？改
- 想加 `decisions.md` `interview-notes.md`？加
- 想把 SAGE 改名 STAR / 5W / Job Story？改 — 但记得改 `AGENTS.md` 的 SAGE
  workflow 段（marker 之外那段是你的）让 AI 跟上
- `marker` 想删？删 — luban 不会再 sync 那段。`luban specs sync` 检测到 marker
  缺失就跳过。

## 不要做的事

- 不要把代码 / API 文档塞进来 — 那归 `src/` + `luban doc`（doxygen）
- 不要把 commit log 风格塞进来 — 那归 git
- 不要写 "TODO 待补" — 空着比假填好（AI 看到空字段会问；看到 "TODO" 会跳过）

## FAQ

**Q：我的项目已经有 README/CONTRIBUTING.md/docs/，跟 specs/ 冲突吗？**

不冲突。specs/ 是给 AI 用的，README 是给人用的。两者关注点不同：specs 强调
"为什么"和"约束"，README 强调"怎么开始"。

**Q：`luban specs sync` 会覆盖我对 AGENTS.md 的修改吗？**

不会。只覆盖 `<!-- BEGIN luban-managed -->` 到 `<!-- END luban-managed -->`
之间的内容；marker 外面的修改 100% 保留。

**Q：能在 monorepo 里给每个子项目独立 specs/ 吗？**

能。`luban specs init` 默认在 cwd 操作（向上找 `vcpkg.json` 或 `CMakeLists.txt`
做项目根）。在子项目目录里跑就行。

**Q：能不带 SAGE 直接用 AGENTS.md 部分吗？**

能。`luban specs init --no-sage`（计划中）只落 AGENTS.md / CLAUDE.md，不建
specs/ 目录。如果你不需要"场景驱动需求"流程，只想给 AI 一个项目体检报告，
单 AGENTS.md 就够。

**Q：marker block 是什么协议？**

`<!-- BEGIN luban-managed: <section-name> -->` 到 `<!-- END luban-managed -->`
之间。section-name 是 kebab-case 字符串（`project-context` /
`cpp-modernization` / `ub-perf-guidance` 等）。`luban specs sync` 解析这些
marker，重写块内内容；外面原文不动。删 marker = 退出 luban 管理那段。
