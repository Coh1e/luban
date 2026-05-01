# 测试策略

## 现状

- **没有正式单元测试**。早期 `tests/*_smoke.cpp`（hash_smoke 等）已在验证后删除。
- 验证途径以**手测路径**为主：
  - `luban.exe doctor` 自检
  - 在 tmp 目录跑 `new → add → build`
  - CI 在 GitHub Actions Windows runner 跑 `cmake --build --preset release` 并发版

## 计划（M3+）

| 层 | 目标 | 工具 |
|---|---|---|
| 单测 | 关键纯函数（`hash`、`vcpkg_manifest`、`luban_cmake_gen::render`、`paths::resolve`） | doctest（single-header，符合 "无新依赖" 原则） |
| 集成测 | end-to-end `new → add → build` 在 isolated `LUBAN_PREFIX` | bash + 临时目录 |
| 校验测 | `installed.json` schema 兼容（v1 → v2 readback） | doctest |

## 测试不变量

- **不要 mock 文件系统**——单测使用临时目录（`std::filesystem::temp_directory_path()`）。
- **不要 mock 网络**——下载相关测试要么走真实小文件、要么 skip。
- **`luban setup` 不在 CI 默认跑**（拉 ~250 MB）；改在专门的 nightly job。

## 烟测脚本（建议）

`scripts/smoke.bat`（占位）：

```bat
@echo off
set TMP_PROJ=%TEMP%\luban-smoke-%RANDOM%
mkdir "%TMP_PROJ%" && cd "%TMP_PROJ%"
luban new app smoke || exit /b 1
cd smoke
luban add fmt || exit /b 1
luban build || exit /b 1
build\default\smoke.exe || exit /b 1
echo OK
```

放到 CI 的 release job 出 artifact 后跑一次。
