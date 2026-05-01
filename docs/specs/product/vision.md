# 愿景

> **一句话**：让 Windows 上写 C++ 像 `rustup + cargo` 一样安静。

## 现实痛点

新机器装一套现代 C++ 工具链平均要 1–3 天：LLVM-MinGW / cmake / ninja / clangd /
vcpkg / git / 触发 `find_package`/`CMakePresets`/`triplet`/`vcpkg.json` 等等胶水
配置。每台新机器、每个新人都要重走一遍。MSVC + vcpkg classic 模式更糟，因为大量
依赖隐式 PATH 与 IDE 注入。

## 我们的赌注

提供 **一个 3 MB 的 `luban.exe`**，把上述胶水全部吃掉：

```bat
luban setup && luban env --user        :: 一次性，约 3 分钟，~250 MB
luban new app foo && cd foo
luban add fmt && luban build           :: 用户从未打开 CMakeLists.txt
```

但 **不取代** cmake / ninja / vcpkg / git——它们仍然是主体，luban 只是辅助胶水。
项目克隆到没装 luban 的机器，`cmake --preset default && cmake --build --preset
default` 仍能编过。

## 长期目标（北极星）

1. **Windows-first 但天然支持 Linux/macOS 移植**（XDG-first 路径设计已经预留）。
2. **零 UAC、零系统级写入**——每个文件都在 `HKCU` + 用户家目录。
3. **可逆**——卸载 = `luban self uninstall` + 删一行 `include(luban.cmake)`。
4. **AI 友好**——每个 verb 都有结构化 `--json` 输出，便于 IDE/agent 集成。

## 价值主张（按受众）

| 受众 | 价值 |
|---|---|
| 个人 / 学生 | 5 分钟从零跑起一个 hello-world，含包管理 |
| 团队 onboarding | 新人 `luban setup` 然后克隆仓库就能开发 |
| CI / 容器 | `LUBAN_PREFIX=/somewhere luban setup` 可重复构建环境 |
| AI agent / IDE | `luban describe --json` 提供完整项目状态 |
