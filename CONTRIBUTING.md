# 贡献指南

欢迎贡献！luban 仍在快速演进，请先读 [`CLAUDE.md`](./CLAUDE.md) 与 [`docs/specs/`](./docs/specs/)
熟悉项目方向。

## 准备开发环境

```bat
:: 用现成 luban 装套工具链来 dogfood
luban setup && luban env --user
:: 重开 shell

git clone https://github.com/Coh1e/luban && cd luban
cmake --preset default
cmake --build --preset default
build\default\luban.exe --version
```

## 改动检查清单

- [ ] 阅读 `CLAUDE.md`、`docs/specs/architecture/module-boundaries.md`
- [ ] 改 verb / schema / 公共 API → 先开 ADR（`docs/specs/decisions/ADR-XXXX-*.md`）
- [ ] 不引入新第三方依赖（除非 single-header vendor 到 `third_party/`；不嵌 Rust crate，详见 ADR-0002）
- [ ] 跑 `luban doctor --strict` + 在 tmp 目录手测 `new → add → build`
- [ ] 文档同步更新（mdBook 在 `docs/src/`，治理文档在 `docs/specs/`）
- [ ] **代码注释质量符合 ADR-0002**（详细 + WHY + doxygen brief）
- [ ] commit 信息遵循现有风格（看 `git log --oneline`）

## 提 PR

1. fork → branch（`feat/xxx` / `fix/xxx`）
2. **每个独立改动一次 PR**（recipe 卡片粒度）
3. PR 描述里附**手测路径**与**变更影响范围**
4. 等 CI（Windows runner）通过

## 文档贡献

- 用户手册 → `docs/src/`（mdBook，会自动发到 luban.coh1e.com）
- 设计/治理文档 → `docs/specs/`
- API 文档（Doxygen）→ 直接写注释；CI 自动生成

## 中文 / 英文

主线为中文（项目背景）+ 英文（命令、API、错误消息）。新文档默认中文，但具体技术
术语保留英文（如 `forward_rest`、`HashSpec`）。

## 安全 / 漏洞

请通过 GitHub Security Advisory 私下报告，不要开 public issue。
