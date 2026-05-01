# 未决问题（Open Questions）

> 列出已知但还没拍板的事。每条问题保持 "问题 + 候选 + 偏好（可空）" 三段式，便于
> 季度 review 时判断是否升 ADR。

## OQ-1：MSVC 工具链支持？ — Phase 1 ✅ 已 (v0.2)

- **背景**：原始问题——luban 只构建 + 分发 LLVM-MinGW；MSVC 是闭源 + 大体积 +
  安装时需登录 Microsoft 账户。
- **v0.2 解法**：`luban env --msvc-init` 走 vswhere 探测既有 MSVC 安装 + capture
  vcvarsall env 到 `<state>/msvc-env.json`，spawn 子进程时 env_snapshot 注入。
  **不接管 MSVC 安装**——用户用 Microsoft 自家 installer 装。
- **遗留 (Phase 2)**：把 capture 的 env 写到 HKCU 让 fresh shell 也能裸跑 cl.exe。
  代价是 ~30 个 HKCU env，与"limit PATH pollution"原则张力大；先观察 Phase 1 用户
  反馈再决定。

## OQ-2：per-project toolchain pin？

- **背景**：rustup 风格的 `rust-toolchain.toml`。当前所有项目共享 system-tier toolchain。
- **候选**：
  - A. `luban.toml [toolchain] cmake = "4.3.2"` 加重新选 / 安装
  - B. workspace 级而非 project 级
- **偏好**：未定。先观察 OQ-3。

## OQ-3：workspace 支持？

- **背景**：cargo workspace 风格。多 sibling 项目共享 build / vcpkg cache。
- **候选**：
  - A. `luban.toml [workspace] members = [...]`
  - B. 不做，让用户用 cmake superbuild
- **偏好**：A，但优先级低于 OQ-1 / OQ-4。

## OQ-4：Linux/macOS port 时间表？

- **背景**：架构已预留（XDG-first、`paths.cpp` 平台分支）；阻塞点是 `proc.cpp`
  的 POSIX 实现 + `download.cpp` 换 libcurl + 生态适配。
- **候选**：
  - A. 用 `LUBAN_PREFIX` 跑容器内 Linux 实验，先满足 CI 可用
  - B. 完整移植 + 发布 Linux/macOS 二进制
- **偏好**：A 先做，B 看用户量。

## OQ-5：`describe --json` 是否纳入稳定 schema？

- **背景**：IDE/agent 集成需要稳定结构。一旦稳定下来变更代价大。
- **候选**：
  - A. 标 schema=1，只增不删，破坏需 ADR
  - B. 维持 unstable 标记一段时间
- **v0.2 进展**：`describe --json` / `describe --host --json` / `doctor --json` /
  `<state>/msvc-env.json` 都已经标 `schema: 1`。距离正式承诺只差一份 ADR。
- **偏好**：A，等 ADR-0002/0003 落地时一并升级。

## OQ-6：`lib_targets` 表如何长期维护？

- **背景**：当前 125 条目（v0.2）人工维护。vcpkg 有 `usage` 文件可机器化抽。
- **候选**：
  - A. 写一个一次性脚本扫 vcpkg-registry 生成
  - B. 维持人工
- **偏好**：A，但需要保留对 alias / 多 target 名的人工覆写。

## OQ-7：luban "小生态"（crate 化）+ vcpkg 并行兼容

- **背景**：用户希望 C++ 项目有 Rust crate 那种 first-class 包生态，与 vcpkg 互
  补。`luban add <pkg>` 解析顺序：vcpkg 命中 → 直接装 vcpkg port；vcpkg 缺货 →
  fallback luban registry。luban registry 托管：
  - C/C++ 原生 wrapper（vcpkg 缺货时补位；早期候选：`libjq`——vcpkg 只出 binary 不出 lib）
  - Rust 明星库 FFI wrapper（如 `reqwest-cpp`/`tokio-cpp`/`serde-cpp`，cxx-bridge / cargo-c 包成 C ABI 静态库 + 头文件）
  - 项目模板
  - 工具链 manifest（已有）
- **关键约束**：vcpkg 是项目依赖唯一真相不变（luban registry 安装的产物也照样
  emit 到 cmake `find_package` + `target_link_libraries`，对用户透明）。
- **状态**：M4+ 起草 ADR-0004 详细设计。

## OQ-8：FFI 边界 — luban.exe 自身 vs luban-managed packages

- **背景**：用户对 FFI 的提议是给**用户项目**包 Rust 库（cxx-bridge / cargo-c），
  而不是把 Rust crate 嵌进 luban.exe 自己。这两层 scope 完全不同：
  - luban.exe：保 3MB 单 exe + 0 运行时依赖 + 不引 Rust toolchain
  - luban-managed packages（OQ-7 的 luban registry 包）：允许 Rust 依赖
- **状态**：ADR-0002 (PR 11) 锁定。本文件做指针。
