# ADR-0004 — Vendor doctest as the unit test framework

- **状态**：Accepted (M3.5)
- **日期**：2026-05-01
- **作者**：Coh1e + Claude
- **取代**：n/a

## Context

luban 当前 0 个 C++ 单元测试。test-strategy.md 一直把"单测：doctest"列为 M3+ 计划，
但 ADR-0001 invariant 1（不引第三方依赖，除非 single-header vendor 到 third_party/）
+ ADR-0002 scope (1)（luban.exe 自身不嵌 Rust crate / 不引运行时依赖）一起约束了
"任何新依赖必须先 ADR"。这条 ADR 就是补这个流程，让单测能开张。

具体痛点：

- 关键的纯函数（`hash::HashSpec parse / verify_file`、`paths::resolve` /
  `expanduser`、`luban_toml::cpp` 校验、`scoop_manifest::parse`、
  `lib_targets::lookup`、AGENTS.md marker block engine）现在只能靠**手测**或
  `scripts/smoke.bat` 间接验证。
- v0.2 修过的 bug（如 `X_VCPKG_REGISTRIES_CACHE` 指向不存在目录）单测能在 PR 阶段
  就截住，比靠 smoke 在合并后才发现要快。
- 重构（特别是 OQ-7 luban registry / OQ-2 toolchain pin / OQ-4 Linux 移植）会
  动到核心模块，没单测兜底回归风险大。

## Decision

Vendor **[doctest](https://github.com/doctest/doctest)** 作为 luban 的单元测试
框架。Single-header（一个 `doctest.h` 文件）、MIT 许可证，与 ADR-0001 invariant 1
完全兼容。

### 为什么是 doctest（而不是 Catch2 / Boost.Test / gtest）

| 候选 | 单 header | License | 编译时间 | 选择理由 |
|---|---|---|---|---|
| **doctest** | ✅ ~12k 行单文件 | MIT | 极快（设计目标）| 选 |
| Catch2 v3 | ❌ 拆成多 lib，需 cmake target | BSL-1.0 | 中等 | 不符合"single-header" |
| Catch2 v2 | ✅ | BSL-1.0 | 慢（编译 ~5×) | 已停维护 |
| Boost.Test | ❌ 一大坨 boost 子库 | BSL-1.0 | 慢 | 与"无 boost"原则冲突 |
| GoogleTest | ❌ 多源 + lib | BSD | 中等 | 不符合"single-header" |

doctest 的额外优势：

- **Header-only + 编译时间 ~3×fastest**（doctest 自家 benchmark）
- **API 跟 Catch2 几乎一样**（`TEST_CASE` / `CHECK` / `REQUIRE` / `SUBCASE`），
  生态熟悉的人无切换成本
- **DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN** 宏自带 main，省一份 boilerplate
- **跨平台编译验证过 MSVC / clang / gcc / mingw**，跟 luban 的 LLVM-MinGW 默认
  toolchain 兼容

### Vendor 政策

- **路径**：`third_party/doctest.h`（与 `json.hpp` / `miniz.{h,c}` / `toml.hpp` 同位）
- **License**：`third_party/doctest.LICENSE`（MIT 全文 + copyright header）
- **版本**：从 [doctest releases](https://github.com/doctest/doctest/releases) 取
  最新稳定 tag。**升级要走 PR**，不自动 sync。
- **修改**：禁止本地 patch；如发现 bug 走上游修。

### 测试 target

`luban-tests` 作为独立 cmake 可执行 target，**EXCLUDE_FROM_ALL**：

```cmake
option(LUBAN_BUILD_TESTS "Build luban-tests target (default ON; off-ALL)" ON)
if(LUBAN_BUILD_TESTS)
    add_executable(luban-tests EXCLUDE_FROM_ALL
        tests/test_main.cpp
        tests/test_paths.cpp
        tests/test_hash.cpp
        tests/test_luban_toml.cpp
        tests/test_lib_targets.cpp
        tests/test_marker_block.cpp
        # ... per-module test files
        ${LUBAN_TEST_DEPS_OBJS}    # link against the same .obj files as luban
    )
    target_include_directories(luban-tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party
        ${CMAKE_BINARY_DIR}/gen)
endif()
```

EXCLUDE_FROM_ALL 让默认 `cmake --build` 不编译 tests，开发者主动 `cmake --build
--target luban-tests && ./build/release/luban-tests` 才跑。CI 在 release pipeline
后单独跑一步。

## 拒绝的替代方案

### A. 完全不做 C++ 单测，只靠 smoke

- 优：zero-deps，进一步简化构建。
- 劣：smoke 是端到端，bug 离根因 4-5 层栈深；单测能精准锁定到函数级。重构信心低。

### B. 自实现一个微测框架

- 优：贴合 ADR-0002 "重写 > wrap"，不引外部代码。
- 劣：API 表面 + 报告格式 + tag/filter 等等，自己写要数百行加上长期维护。doctest
  已经做得好，重新发明轮子不是好用 ROI 的地方。

### C. 用 Catch2 v3 / GoogleTest

- 优：生态可能更熟。
- 劣：不是 single-header，要么 vendor 整个 lib（违反 invariant 1）要么 fetch
  external（违反"无运行时网络依赖"原则）。

### D. doctest 但不 vendor — fetch 时拉

- 优：repo 不带这一坨。
- 劣：违反"single-header vendor 到 third_party/" 的明确规则。CI 离线 / 受限网络
  环境会卡。

## 后果

- **正面**：单测开张；future PR 可以加 case，重构有兜底。
- **负面**：repo 多 ~12k 行 vendored code（`third_party/doctest.h`）+ LICENSE 文件；
  任何 lint/scan 工具会把 third_party/ 视为外部代码（标准做法即可）。
- **维护负担**：上游版本更新 = 整个 doctest.h 替换，~1 PR/年频率。

## 实施计划

落地 PR（在本 ADR 合并后开）：

1. `third_party/doctest.h` + `third_party/doctest.LICENSE`（手 vendor）
2. `tests/test_main.cpp` — `#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` + include
3. `tests/test_lib_targets.cpp` — `lookup("fmt")` / 多 target / 未命中 fallback
4. `tests/test_hash.cpp` — `HashSpec parse` 各种 prefix / `verify_file` 小文件
5. `tests/test_luban_toml.cpp` — cpp ∈ {17,20,23} 校验、sanitizers 数组
6. `tests/test_marker_block.cpp` — AGENTS.md sync 引擎边界（无 marker / 多
   marker / END 缺失等）
7. `CMakeLists.txt` — `LUBAN_BUILD_TESTS` option + `luban-tests` target
8. `.github/workflows/build.yml` — 加 `cmake --build --target luban-tests &&
   ./build/release/luban-tests` 步

## 验证

ADR 接受后：

- 上述 PR 合并后 `cmake --build --target luban-tests` 编出 luban-tests.exe
- 跑 `./build/release/luban-tests` 退出码 0
- 第 8 步起 CI 每个 PR 都跑单测 + smoke，回归率显著降低

## 跨页索引

- ADR-0001 — invariant 1（third_party 单 header vendor 政策的源头）
- ADR-0002 — scope (1) luban.exe 不引 Rust（doctest 不冲突，是 C++ 单 header）
- `engineering/test-strategy.md` — 测试策略全图
- `scripts/smoke.bat` — 端到端补充（doctest 单测 + smoke 端到端 = 互补）
