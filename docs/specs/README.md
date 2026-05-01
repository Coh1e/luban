# luban 项目设计文档

本目录是 luban 的 **设计与治理文档**：写给参与者（人或 AI agent）看，回答 "为什么
luban 长这样、怎么演进"。

面向终端用户的安装/使用手册见 mdBook：<https://luban.coh1e.com/>。
贡献者 API 参考见 Doxygen：<https://luban.coh1e.com/api/>。

## 目录

| 区 | 内容 | 何时读 |
|---|---|---|
| `product/` | 愿景、需求、范围 | 决定一个新功能要不要做、做到什么程度 |
| `architecture/` | 总体架构、模块边界、技术栈 | 实现前先对齐 |
| `domain/` | 领域模型、事件模型 | 新增/修改核心实体（Component/Project/Target…）时 |
| `modules/` | 各模块详细说明（按 `module-template.md` 写） | 修改/新增模块时 |
| `interfaces/` | 对外接口（CLI、文件契约、registry schema） | 改动会被外部依赖时 |
| `engineering/` | 本地开发、测试、发布流程 | 加入项目、发版前 |
| `decisions/` | ADR — 不可逆/重大决策的存档 | 决策时新增、回看时检索 |
| `planning/` | 路线图、未决问题 | 季度规划、领任务 |

## 维护守则

- **mdBook 与本目录互不重复**：mdBook 写"怎么用"，本目录写"为什么这么设计"。
- **代码即真相**：本目录里的文件路径、函数名都应可在仓库 grep 到；如无法 grep
  到说明文档已陈旧，需更新或删除。
- **ADR 一旦合入不再修改**，仅以新 ADR 取代（标注 `Supersedes: ADR-XXXX`）。
- **路线图允许漂移**：`planning/milestones.md` 与 `planning/open-questions.md` 与现实对齐即可。
