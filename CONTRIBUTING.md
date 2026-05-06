# 贡献指南

欢迎贡献！请先读 [`CLAUDE.md`](./CLAUDE.md) + [`docs/DESIGN.md`](./docs/DESIGN.md)
熟悉项目方向。决议表见 `docs/DESIGN.md` §24.1（字母序 A → AB）。

## 准备开发环境

```bat
:: 用现成 luban 自举工具链
luban bp apply embedded:cpp-base
luban env --user
:: 重开 shell

git clone https://github.com/Coh1e/luban && cd luban
cmake --preset default
cmake --build --preset default
build\default\luban.exe --version
```

## 改动检查清单

- [ ] 阅读 `CLAUDE.md` + `docs/DESIGN.md` §13（模块边界）+ §16（动词）+ §14（名词）
- [ ] 改 verb / schema / 公共 API / 不变量 → 先在 `docs/DESIGN.md` §23（决策）+ §24.1（决议表）补论证
- [ ] 不引入新第三方依赖（除非 single-header vendor 到 `third_party/`；不嵌 Rust crate / DLL）
- [ ] 跑 `luban doctor --strict` + 在 tmp 目录手测 `new → add → build`
- [ ] 文档同步更新（设计 `docs/DESIGN.md`；AI agent 指引 `CLAUDE.md` / `AGENTS.md`）
- [ ] **代码注释质量符合 §20 注释规范**（详细 + WHY + doxygen brief）
- [ ] commit 信息遵循现有风格（看 `git log --oneline`）

## 提 PR

1. fork → branch（`feat/xxx` / `fix/xxx`）
2. **每个独立改动一次 PR**（recipe 卡片粒度）
3. PR 描述里附**手测路径**与**变更影响范围**
4. 等 CI（Windows + Linux + macOS runner）通过

## 文档贡献

- 唯一设计文档 → [`docs/DESIGN.md`](./docs/DESIGN.md)
- AI agent 指引 → [`CLAUDE.md`](./CLAUDE.md) + [`AGENTS.md`](./AGENTS.md)
- 长版 `--help` 源（manual-grade verbs）→ `templates/help/<verb>.md`（构建时 embed）

## 中文 / 英文

主线为中文（项目背景与设计讨论）+ 英文（命令、API、错误消息）。新文档默认中文，
但具体技术术语保留英文（如 `forward_rest`、`HashSpec`、`extension_registry`）。

## 安全 / 漏洞

请通过 GitHub Security Advisory 私下报告，不要开 public issue。
