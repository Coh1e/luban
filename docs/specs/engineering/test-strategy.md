# 测试策略

## 现状

- **没有正式 C++ 单元测试**——doctest 集成尚未引入（vendor 第三方需 ADR）。
- v0.2 起有 **`scripts/smoke.bat`** 端到端脚本，9 步覆盖全 daily-driver 路径：
  scaffold → add/remove → build → run → specs init/new/status → target add/build → doctor.
- CI 在 GitHub Actions Windows runner 跑 `cmake --build --preset release` 并发版。
- 验证途径以**手测路径** + smoke 脚本为主。

## 计划（M3+ → M4）

| 层 | 目标 | 工具 | 状态 |
|---|---|---|---|
| 端到端 | `new → add/remove → build → run → specs → target → doctor` | `scripts/smoke.bat`（POSIX 版 `smoke.sh` 是 M4 placeholder） | ✅ v0.2 |
| 单测 | 关键纯函数（`hash`、`vcpkg_manifest`、`luban_cmake_gen::render`、`paths::resolve`、`scoop_manifest::parse`、`lib_targets::lookup`、AGENTS.md marker block 引擎） | doctest（single-header，待 ADR 后 vendor） | 🔜 M3.5 |
| 集成 | isolated `LUBAN_PREFIX` 容器内跑 setup → smoke 全流程 | bash + 临时目录 | 🔜 M3.5 |
| 校验 | `installed.json` schema 兼容（v1 → v2 readback）；luban.cmake schema 兼容 | doctest | 🔜 M4 |

## 测试不变量

- **不要 mock 文件系统**——单测使用临时目录（`std::filesystem::temp_directory_path()`）。
- **不要 mock 网络**——下载相关测试要么走真实小文件、要么 skip。
- **`luban setup` 不在 CI 默认跑**（拉 ~250 MB）；改在专门的 nightly job。
- **smoke 不依赖 vcpkg 网络**——v0.2 实测中 vcpkg 拉 PowerShell as build helper 在
  受限网络挂；smoke 验证 `luban add fmt` 编辑 manifest 成功后立刻 `remove`，build
  走的是无 deps 路径。完整 vcpkg roundtrip 留给 nightly。

## smoke 脚本

`scripts/smoke.bat`（实际运行；非 placeholder）：

- 9 步、约 1 分钟，零 vcpkg 网络依赖。
- 路径：`<repo>/scripts/smoke.bat`
- 失败时项目目录保留在 `%TEMP%\luban-smoke-<rand>` 供 postmortem。
- 退出码 = 失败步骤号；0 表示全部通过。

CI 用法（建议放在 release job 出 artifact 后）：

```yaml
- name: Smoke test
  shell: cmd
  run: scripts\smoke.bat
```

POSIX 版（`scripts/smoke.sh`）当前是 placeholder，等 M4 Linux/macOS 移植后镜像。
