# 里程碑（Milestones）

> 与 mdBook 的 `docs/src/architecture/roadmap.md` 同源；本页是简表，roadmap 页是详描。

| 里程碑 | 状态 | 主要交付 |
|---|---|---|
| **M1** | ✅ 完成 | C++ 端口 `doctor` `env` `new` `build` `shim`；单 static `luban.exe` |
| **M2** | ✅ 完成 | `setup` 全套（WinHTTP/BCrypt/miniz、Scoop manifest 安全过滤、bucket sync、vcpkg bootstrap）；Python 退役 |
| **M2.5** | ✅ 完成 | cmake/vcpkg 辅助前端：`add/remove/sync`、`target add/remove`、`luban.cmake` v1 生成器、`luban.toml` v1、~50 lib 映射、HKCU env 写入 |
| **M3 (v0.2)** | ✅ 完成 | env/HKCU 收敛（删 LUBAN_*、删 .ps1/.sh、删 activate scripts）；bucket_sync 移除 + manifest_source 替代；CMakeLists VERSION 单一真相；`install.ps1` (uv 风格)；`doctor --strict --json`；`luban doc` (doxygen)；`cpp` 校验；`env --print` (CI 用)；perception 模块；mirror 回退；MSVC vcvarsall capture (Phase 1) |
| **M3.5** | 🔜 进行中 | SAGE / AGENTS.md 渲染系统 (PR 15)；ADR-0002/0003 治理文档；MSVC Phase 2 (HKCU 写入)；chunked Range download |
| **M4+** | 🌅 开放 | workspace、per-project toolchain pin、Linux/macOS port、TUI、`luban tool install`、libluban-core 抽离、luban registry (OQ-7) |

## 提案流程

新里程碑 / 推迟现有里程碑 → 在 `../decisions/` 开 ADR，引用本表。本表只反映 "已通过决策" 的状态。
