# ADR-0001 — 初始架构选型

- **状态**：Accepted（v1）
- **日期**：2025-Q4（首次提案）/ 2026-04-30（归档）
- **作者**：Coh1e + Claude
- **取代**：n/a

## 背景

Windows C++ 工具链装配现状：MSVC + vcpkg classic 隐式 PATH 一团；MinGW 各发行版差
异巨大；vcpkg manifest mode 学习成本高。诉求是**让一个新人在新机器上 5 分钟内能开
始写 modern C++**。

## 决策

构建一个 **Windows-first** 的 C++23 单文件工具，承担：

1. 工具链分发（LLVM-MinGW、cmake、ninja、mingit、vcpkg、emscripten）
2. 项目脚手架（`luban new`）
3. 依赖编辑（`luban add` → `vcpkg.json`）
4. cmake 胶水（生成 `luban.cmake`）
5. 透明构建（`luban build`）

**不**做：build 系统、IR、新 manifest、IDE 插件、HKLM/Program Files 写入。

## 8 条不变量（详见 `../architecture/overview.md`）

详见架构概述页。简言之：cmake 主体、`luban.cmake` git-tracked、`vcpkg.json` 唯一
依赖来源、XDG-first 路径、零 UAC、单 static 二进制、两层依赖严格分离。

## 被拒绝的替代方案

- **A. luban DSL → lower 到 cmake**（xmake/meson 路径）：用户最终仍需懂 cmake，
  逃生通道粗糙。
- **B. CMakeLists.txt 内 marker block 编辑**：边界脆弱，hand-edit 易冲突。
- **C. 全 unified `luban.toml`**（cargo 风格）：与 vcpkg.json 双重真相。
- **D. `luban-init.exe` bootstrapper**：luban.exe 本身已是单文件，多一层无价值。

详见 mdBook 源 `../../src/architecture/why-cmake-module.md`。

## 后果

- **正面**：用户可在删 `include(luban.cmake)` 后退出 luban；项目跨机器只需
  cmake + vcpkg。
- **负面**：M4 才考虑 Linux/macOS；首批用户只能在 Windows 上验证。
- **维护负担**：`lib_targets` 表需手工维护（123 条目，覆盖前 ~50 popular libs）。

## 验证

- `luban setup` + `new` + `add fmt` + `build` 在 Win10/11 干净 VM 上跑通
- 把项目克隆到无 luban 机器、装系统 cmake/vcpkg，`cmake --preset default && cmake
  --build --preset default` 仍能编过

## 跨页索引

- `../architecture/overview.md`
- mdBook 源：`../../src/architecture/{philosophy,two-tier-deps,why-cmake-module,design}.md`
