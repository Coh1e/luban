# ADR-0002 — 分层策略：重写 / wrap / FFI 边界 + 代码注释规范

- **状态**：Accepted (v0.2)
- **日期**：2026-05-01
- **作者**：Coh1e + Claude
- **取代**：n/a

## Context

luban 将"自己用什么实现"和"给用户项目用什么实现"两件事不区分地讨论时，会出现矛盾的
直觉：

- 一边主张"3 MB 单 static-linked 二进制 + 0 运行时依赖"——这是 luban.exe 自身的
  invariant，写进 ADR-0001 的 8 条不变量里。
- 另一边主张"应当能让用户用上 Rust 明星库（reqwest / tokio / serde / hyper）"——
  这是 OQ-7（luban 小生态）的核心方向。

混淆这两层会得出错误结论：要么"luban.exe 嵌 reqwest 不行，所以 luban 不应该提供
reqwest"（牺牲了用户项目的能力），要么"为了让用户能用 reqwest，luban.exe 也得能
嵌 Rust crate"（破坏单 exe 不变量）。

ADR-0001 alt D（拒绝 init.exe）的逻辑同源——但它只覆盖了"luban 自身"层。

本 ADR 把两层 scope 显式分开，并且把**重写 / wrap / FFI** 三阶策略分别绑定到对应层。

## Decision

### Scope (1)：luban.exe 自身

策略层级（从高到低偏好）：

1. **重写**（写进 `src/`）
   - 当前实现：download / hash / archive / proc / paths / scoop_manifest /
     manifest_source / shim / shim_exe / win_path / registry / luban_toml /
     luban_cmake_gen / vcpkg_manifest / lib_targets / msvc_env / perception /
     env_snapshot / cli。
   - 总共 ~6.5k 行 C++23。

2. **vendor single-header wrap**（写进 `third_party/`）
   - 当前 vendor：`json.hpp` (nlohmann/json, MIT) / `miniz.{h,c}` (BSD-3) /
     `toml.hpp` (toml++, MIT)。
   - 必须 single-header（或 single-{c,h} 对）+ committed LICENSE。

3. **FFI 不允许**
   - 不嵌 Rust crate（reqwest / sha2 / zip / indicatif / ratatui 等）。
   - 不引第三方 .dll / .so / .dylib 链接。
   - 静态链接 `-static -static-libgcc -static-libstdc++` 是 ADR-0001 invariant 6 的
     强约束。

**不变量**：3 MB release / static-linked / 0 第三方运行时依赖 / 不引 Rust toolchain
作构建依赖。

### Scope (2)：luban-managed packages（luban registry，OQ-7）

策略层级：

1. **wrap vcpkg port** 优先
   - 用户写 `luban add fmt` → 命中 vcpkg port → 用 vcpkg 装。
   - luban registry 不重复 vcpkg 已有的内容。

2. **wrap C/C++ 原生 lib**（vcpkg 没收的）
   - 例：`libjq`（vcpkg 的 jq port 只出 binary 不出 lib）。
   - luban registry 提供一个 vcpkg-port-shape 的 manifest + 编译规则。

3. **FFI 包 Rust 明星库**（兜底）
   - 例：`reqwest-cpp` / `tokio-cpp` / `serde-cpp`。
   - 工艺：cxx-bridge / cbindgen / cargo-c 三选一，包成 C ABI staticlib + 头文件。
   - 用户项目 `#include <reqwest.h>` 然后 `target_link_libraries(... reqwest::reqwest)`。
   - 用户项目知道里面是 Rust？无所谓——只看到 C++ API。

**这层允许 Rust 依赖**：因为 luban registry 是用户项目的运行时事，跟 luban.exe
的单 exe invariant 无关。luban registry 包构建时需要 cargo / rustc，那是 OQ-7 工
艺要解决的；luban.exe 本身不引这个。

## 拒绝的替代方案

### A. 一刀切"luban 圈不引 Rust"

- 优：完全规避 Rust toolchain 复杂度。
- 劣：放弃了 reqwest / tokio 这种 Rust 已经做得比 C++ 同类好得多的库。用户被迫
  用 cpr / boost::beast 等弱替代。

### B. 一刀切"允许 luban.exe 引 Rust crate"

- 优：可以把 reqwest 嵌进 luban.exe 的 download.cpp。
- 劣：拖入 ~10-20 MB Rust runtime；构建依赖 cargo + rustc；破坏 ADR-0001 invariant 6
  和 7。

### C. luban registry 强制 vcpkg 上游接收

- 优：不分裂生态。
- 劣：vcpkg 的接收节奏 + 边界很慢；FFI 包 Rust 这种"我们想做"vcpkg 不接收。

## 后果

- **正面**：luban.exe 永远是 single static binary；用户项目能消费 Rust 生态长板；
  vcpkg 是项目依赖唯一真相不变（luban registry 包也 emit `find_package` 给 cmake）。
- **负面**：维护两个不同 scope 的 build / packaging pipelines（luban.exe 是 cmake
  + LLVM-MinGW；luban registry 包根据库类型走不同工艺）。
- **维护负担**：luban registry 起草后，每个 wrap 包要追上游版本——是 OQ-7 设计要
  解决的（probably git submodule + 季度 sync）。

## 注释规范（Hard Rule，所有 v0.2+ 提交都遵守）

luban 自己的代码必须有**详细注释**。具体：

1. **每个 .cpp 顶部**有一段块注释，说明：
   - 模块对外承诺什么（一句话职责）
   - 为什么是现在这个形态（如果不是显然的）
   - 历史/演进决策（如"v0.1.x 是 X，v0.2 改成 Y，因为 …"）

2. **每个公共函数 / 类 / 结构**：
   - 一行 doxygen 风格 brief（"What it does"）
   - 必要时 `@param` / `@return` 描述非显然的语义
   - 复杂返回类型（含 `expected<T, E>`）必须标注 E 的语义

3. **复杂分支 / 状态机 / 边界条件 / workaround**必须有 inline 注释，**说 WHY**：
   - "Why is this `if` check here?"
   - "Why this specific order?"
   - "What broke last time we did the obvious thing?"

4. **删代码同步删注释**——防 stale。

5. **AI agent 输出代码必须配详细注释**，不得省略。AGENTS.md 写明这条。

不接受的反例：

```cpp
// Increment x
x++;

// Return the value
return x;
```

接受的正例：

```cpp
// Pre-decrement so the wraparound case (x == 0) returns INT_MAX, not -1 —
// callers iterate while (--x >= 0) and rely on the unsigned roll-over.
--x;
```

## 验证

- AGENTS.md 顶部加一条"代码注释 quality"section 明确这个标准。
- CONTRIBUTING.md 改动检查清单加一条 "代码注释质量符合 ADR-0002"。
- 此 ADR 之后的 PR review 把"注释是否解释了 WHY"作为合格标准。

## 跨页索引

- ADR-0001 — 初始架构选型（8 条不变量的源头）
- OQ-7 — luban registry / 小生态
- OQ-8 — FFI 边界 scope 明确化
- `../architecture/module-boundaries.md` — 不变量列表
