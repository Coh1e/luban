# 里程碑（Milestones）

> 与 mdBook 的 `docs/src/architecture/roadmap.md` 同源；本页是简表，roadmap 页是详描。

| 里程碑 | 状态 | 主要交付 |
|---|---|---|
| **M1** | ✅ 完成 | C++ 端口 `doctor` `env` `new` `build` `shim`；单 static `luban.exe` |
| **M2** | ✅ 完成 | `setup` 全套（WinHTTP/BCrypt/miniz、Scoop manifest 安全过滤、bucket sync、vcpkg bootstrap）；Python 退役 |
| **M2.5** | ✅ 完成 | cmake/vcpkg 辅助前端：`add/remove/sync`、`target add/remove`、`luban.cmake` v1 生成器、`luban.toml` v1、~50 lib 映射、HKCU env 写入 |
| **M3 (v0.2)** | ✅ 完成 | env/HKCU 收敛（删 LUBAN_*、删 .ps1/.sh、删 activate scripts）；bucket_sync 移除 + manifest_source 替代；CMakeLists VERSION 单一真相；`install.ps1` (uv 风格)；`doctor --strict --json`；`luban doc` (doxygen)；`cpp` 校验；`env --print` (CI 用)；perception 模块；mirror 回退；MSVC vcvarsall capture (Phase 1) |
| **M3.5 (v0.3)** | ✅ 完成 | SAGE / AGENTS.md 渲染系统 + marker block 引擎单测 (PR 15)；ADR-0002/0003/0004/0005/0006 治理文档；MSVC Phase 2 (HKCU 写入)；chunked Range download；doctest 25 → 40 个用例 (109 断言)；`lib_targets` 125 → 224 条；`[toolchain]` pin (OQ-2 Phase 1) |
| **M4** | 🌅 开放 | Linux/macOS port Phase A/B/C ([ADR-0006](../decisions/ADR-0006-posix-port.md))；luban registry Phase 1（toolchain 迁移到 [ADR-0005](../decisions/ADR-0005-luban-registry.md) index repo）；workspace；TUI；libluban-core 抽离 |
| **M4.5+** | 🌅 远期 | luban registry Phase 2（lib + lib-rs：reqwest-cpp / libjq）+ Phase 3（template）；`luban tool install`；OQ-2 `--strict-pins` |

## 提案流程

新里程碑 / 推迟现有里程碑 → 在 `../decisions/` 开 ADR，引用本表。本表只反映 "已通过决策" 的状态。
