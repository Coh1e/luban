# 需求

按 **MoSCoW**（Must / Should / Could / Won't）整理。Must 等于已经实现并被 v1.0
契约保护；Should 是进行中或下一里程碑；Could 是尚未承诺。

## Must — v1 已交付

- **R1** 单二进制安装：`luban.exe` ≈ 3 MB，static-linked，无 DLL/Python/MSI。
- **R2** 零 UAC：所有写入都在 `%LOCALAPPDATA%`、`%APPDATA%`、`HKCU`。
- **R3** XDG-first 路径解析：`LUBAN_PREFIX` → `XDG_*_HOME` → 平台默认。
- **R4** 一键工具链：`luban setup` 安装 LLVM-MinGW + cmake + ninja + mingit + vcpkg。
- **R5** 透明工程胶水：`luban new` / `luban add` / `luban build` 流程，用户无需
  编辑 `CMakeLists.txt`。
- **R6** 可逆：`luban self uninstall` + 删 `include(luban.cmake)` 即完全退出。
- **R7** 项目无 luban 也能编：`luban.cmake` 是标准 cmake 模块、git-tracked。
- **R8** 一致的 SHA256 校验：所有下载链路（cache 命中 / 流式下载）都走同一份
  `hash::verify_file`，禁止在 manifest 中出现 installer-script 类构件。
- **R9** Two-tier 依赖：toolchain 通过 `luban setup` 管理（系统级），项目库通过
  `luban add` 管理（写入 `vcpkg.json`，git-tracked）。
- **R10** 16 个 CLI verb，4 组（setup / project / dep / advanced），见
  `../interfaces/api-overview.md`。

## Should — M3 daily-driver polish

- **R11** WASM 一等公民：`luban setup --with emscripten` + `luban new app foo
  --target=wasm` + `luban build` → `.html/.js/.wasm`。**已实现**（提升至 Must 候选）。
- **R12** 完整 shell completion：当前已支持 cmd 的 `clink`，扩展 bash/zsh/pwsh/fish。
- **R13** `luban describe --json` 稳定 schema，可被 IDE 插件消费。
- **R14** `luban toolchain {list,use,install}` 多版本工具链。

## Could — M4+

- **R15** 跨平台：Linux/macOS 移植（`proc.cpp` POSIX、`download.cpp` libcurl 等）。
- **R16** Workspace（多 sibling 项目共享构建/vcpkg 缓存）。
- **R17** Per-project toolchain pin（`luban.toml [toolchain]`）。
- **R18** `luban tool install <pkg>`（pipx 等价物）。
- **R19** TUI / 交互模式（FTXUI）。

## Won't — 明确不做

- 不替换 cmake、不发明 IR、不发明 manifest 格式
- 不写 `Program Files`、不动 `HKLM`、不需 admin
- 不维护官方 IDE 插件（生成 `compile_commands.json` 即可）
- 不做 "luban cloud"、跨机同步、telemetry
- 不支持 MSVC 构建 luban 自身（除非有人来贡献）
