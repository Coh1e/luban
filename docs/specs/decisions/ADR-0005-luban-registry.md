# ADR-0005 — luban registry: vcpkg-complementary C/C++ package source

- **状态**：Proposed (M4+ target — design freeze, no implementation yet)
- **日期**：2026-04-30
- **作者**：Coh1e + Claude
- **关联 OQ**：OQ-7（luban crate 化 + vcpkg 并行兼容）
- **关联 ADR**：ADR-0001（架构约束）、ADR-0002（layered FFI scope）

## Context

vcpkg 现在是 luban 的依赖唯一真相（vcpkg.json + luban.cmake auto-gen）。但
vcpkg 有三类已知缺口：

1. **vcpkg 出"binary-only" 不出 lib**。早期典型：`jq`（vcpkg 只有 binary
   port，不能 `find_package(jq)`）；`pandoc`、`ripgrep` 同理。这些是 **vcpkg
   范畴外**的工具，但用户写 C++ 想"`luban add jq` 然后链接 libjq.a"是合理
   预期。
2. **vcpkg 没有的 Rust 明星库**。`reqwest`（HTTP 客户端，async + TLS）、
   `tokio`（异步运行时）、`serde`（序列化框架）这种**已经存在且工艺成熟的
   Rust crate** 没有等价的 C++ 版本，也没人愿意从零写。理论上 cxx-bridge /
   cbindgen / cargo-c 可以把它们包成 C ABI 静态库 + 头文件，但**包装工程零碎
   且容易腐烂**——如果每个项目都自己包，就是 N 份 `reqwest-cpp` fork。
3. **C++ 项目模板缺生态**。`luban new app` 只能从内置 `templates/` 选；用户
   想"启一个 cmake + spdlog + cli11 + json 的服务模板"得自己手搓。

vcpkg 设计上不会管这些：

- vcpkg 的核心使命是"port 集合 + binary cache"；packing Rust crate 不在它的
  scope 里
- vcpkg 的 port 是**集中维护的**（PR 到 microsoft/vcpkg），ramp-up 慢；luban
  registry 可以更激进、更本地化
- vcpkg 的 manifest schema 不为模板 / 工具 / FFI wrapper 设计

ADR-0001 强调 luban 不引第三方运行时依赖，ADR-0002 在 layered scope 下**明
确允许 luban-managed packages 引 Rust crate（scope 2，用户项目侧）**。
ADR-0005 给出"如何把 scope-2 落地成可用产品"的设计。

## Goals

1. **vcpkg 仍是默认 + 唯一真相**。`luban add fmt` 永远先打 vcpkg；vcpkg.json
   永远是用户项目里依赖列表的源代码级 SSoT。
2. **luban registry 当 fallback / 互补**，**不**是 vcpkg 的替代。命中 vcpkg
   就走 vcpkg；只有 vcpkg 缺货 / 用户显式 `--source=luban` 时才动 luban registry。
3. **luban registry 安装出来的依赖对用户项目透明**：CMakeLists.txt 该用
   `find_package(reqwest)` + `target_link_libraries(... reqwest::reqwest)`，
   就跟 vcpkg port 一样。luban.cmake 的 generated 内容延展到包含 luban registry
   产物，**不要让用户看到两套 import 语法**。
4. **luban registry 的 host / discovery / publish 是开放的**：M4 自己 host 一个
   index repo（GitHub），M5+ 允许第三方 mirror（类似 cargo registries）。
5. **package category** 至少四类：
   - `lib` — C/C++ 原生静态/动态库 wrapper（典型：libjq、librdkafka 当 vcpkg 缺时）
   - `lib-rs` — Rust crate via cxx-bridge / cargo-c 包成的 C ABI 库
   - `template` — 项目模板（cmake skeleton + 推荐 dep set）
   - `toolchain` — 工具链组件 manifest（已经在 manifests_seed/ 实现，迁到 luban registry 后形成统一模型）

## Architecture

### Index repo schema

luban registry 的"目录"是一个 GitHub repo（M4 临时 `Coh1e/luban-registry`，M5+
官方 `luban-build/registry`）。结构：

```
luban-registry/
  index/
    libjq/
      luban.json              # 包元数据（schema=1）
      versions/
        1.7.0.json
        1.6.2.json
    reqwest-cpp/
      luban.json
      versions/
        0.11.27-cxx0.json     # 上游 reqwest 0.11.27 + cxx 包装版本
    template-grpc-server/
      luban.json
    cmake/                    # 既有 toolchain 迁过来
      luban.json
      versions/
        4.3.2.json
  meta/
    schema-1.json             # 索引整体 schema 描述
    catalog.json              # 全包列表 + 类别（用于 luban search）
```

`<pkg>/luban.json` 主声明：

```json
{
  "schema": 1,
  "name": "libjq",
  "category": "lib",
  "description": "JSON command-line processor — wrapped as a static lib + jq.h",
  "license": "MIT",
  "upstream": "https://github.com/jqlang/jq",
  "vcpkg_alias": null,
  "_why_no_vcpkg": "vcpkg only ships jq as a port that builds the cli binary; libjq.a is not exported via find_package."
}
```

`<pkg>/versions/<ver>.json` 装版（每版一份）：

```json
{
  "schema": 1,
  "version": "1.7.0",
  "url": ["https://github.com/Coh1e/luban-registry/releases/download/libjq-1.7.0/libjq-1.7.0-windows-x64-mingw.zip", "..."],
  "hash": "sha256:abc...",
  "extract_dir": "libjq-1.7.0",
  "platforms": ["windows-x64-mingw", "windows-x64-msvc", "linux-x64", "macos-arm64"],
  "find_package": {
    "name": "libjq",
    "targets": ["libjq::jq"]
  },
  "depends_on": [],
  "_provenance": {
    "build_workflow": "https://github.com/luban-build/libjq-build/actions/runs/...",
    "source_commit": "abc1234"
  }
}
```

### Resolution flow — `luban add <pkg>`

```
luban add fmt
   ↓
Step 1: vcpkg search
   ↓
   有 → vcpkg add fmt（写 vcpkg.json）
        ↑ 99% 的库走这条
   ↓
   无 → Step 2: luban registry search
        ↓
        有 → luban registry install + 写 luban.toml [luban-deps] 段
        无 → 报错（提示"vcpkg 没有 / luban registry 也没有；考虑 --source=path 或开 RFE issue"）

luban add jq      → vcpkg 没（只有 jq port 出 binary）→ luban registry 命中 libjq → 装
luban add reqwest → vcpkg 没（Rust 库）              → luban registry 命中 reqwest-cpp → 装
luban add fmt     → vcpkg 命中                        → 走 vcpkg
```

显式 force：`luban add jq --source=luban-registry`（绕过 vcpkg 即使命中）。

### Storage

luban registry 装的产物**不进 vcpkg.json**。它们去哪？

- **toolchain 类**（cmake/ninja/llvm-mingw 等）：进 `<data>/toolchains/`，由
  `installed.json` 索引 — **跟当前 manifests_seed 一样**。这是已经工作的模型。
- **lib / lib-rs 类**：进 `<data>/libs/<pkg>-<ver>/`，由新增的
  `<state>/luban-deps.json` 索引（per-machine）。关键设计：每个 lib 一份独立目录，
  内含 `lib/`、`include/`、`<pkg>-config.cmake`（vcpkg-style 兼容）+
  `<pkg>-config-version.cmake`。
- **template 类**：进 `<data>/templates/<pkg>-<ver>/`，由 `luban new` 直接读。

### luban.toml 新段：`[luban-deps]`

vcpkg.json 是用户项目依赖唯一真相 —— 这条**对 vcpkg 不变**。luban registry 装的
**非 vcpkg 依赖**进 luban.toml 新段 `[luban-deps]`：

```toml
[project]
cpp = 23

[luban-deps]
libjq = "1.7.0"
reqwest-cpp = "0.11.27-cxx0"
```

**"两文件依赖说明"**——双源问题怎么解？关键洞察：vcpkg.json 装的是 vcpkg 那一坨；
[luban-deps] 装的是 vcpkg **没有**的那一坨。两个集合**不重叠**。luban 的 add
verb 在写之前先确认互斥（vcpkg 命中就只写 vcpkg.json，luban registry 命中就只写
[luban-deps]），不会出现一个 dep 同时在两边。

luban.cmake 的 generator 改成扫两边：

```cmake
# luban.cmake 自动生成（don't edit; rerun `luban gen` to refresh）
# vcpkg-tracked deps：
find_package(fmt CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
# luban registry-tracked deps:
find_package(libjq REQUIRED PATHS "${LUBAN_DEPS_ROOT}/libjq-1.7.0")
find_package(reqwest REQUIRED PATHS "${LUBAN_DEPS_ROOT}/reqwest-cpp-0.11.27-cxx0")

luban_link(${ARGV0}
    fmt::fmt
    spdlog::spdlog
    libjq::jq
    reqwest::reqwest
)
```

### Publish flow

luban registry 自身是 GitHub repo + GitHub Releases。新版本走 PR：

1. 任何人开 PR 到 `luban-registry`，加 `<pkg>/versions/<ver>.json`
2. CI（GitHub Actions）跑 `luban-registry-validator`：schema 校验 + url 可达 +
   sha256 匹配 + （对 lib 类）下载并 unit-test 一个 minimal `find_package` 用例
3. Maintainer 合 PR
4. catalog.json 自动重生（GitHub Action）
5. luban search 6 小时内（client cache TTL）拿到新版

### lib-rs（Rust FFI）的具体工艺

reqwest-cpp / tokio-cpp / serde-cpp 的 build pipeline：

```
upstream Rust crate
    ↓
luban-registry 下的"build repo"（如 luban-build/reqwest-cpp）：
    Cargo.toml + build.rs + cxx-bridge / cargo-c 配置
    ↓
GitHub Actions 矩阵 build：
    matrix: [ubuntu-22.04, macos-14, windows-llvm-mingw, windows-msvc]
    每平台产出：reqwest-X.Y.Z-{platform}.zip
    内含：lib/libreqwest.a, include/reqwest.h, share/cmake/reqwest/{config,version}.cmake
    ↓
传 GitHub Releases；luban-registry/reqwest-cpp/versions/X.Y.Z.json 引用
    ↓
luban add reqwest 下下来解压到 <data>/libs/reqwest-cpp-X.Y.Z/
    ↓
luban gen 把 find_package(reqwest PATHS ...) emit 到 luban.cmake
    ↓
用户的 main.cpp #include <reqwest.h>，链 reqwest::reqwest，工作
```

**关键工程约束**：
- 包出来的 lib 必须是**静态 + 自包含**（不要求用户机有 cargo / rustc）
- C ABI header 必须 stable 且**版本匹配**（cxx-bridge 生成的 header 嵌 schema 版本）
- TLS 链：reqwest → rustls → ring → 不依赖系统 OpenSSL（这是 Rust 这条路的**最大优势**——一份静态二进制走天下）

## Decision

执行下述设计；M4 启动 Phase 1（toolchain 迁过去 + index repo 落地），M5 启动
Phase 2（lib + lib-rs），M5.5+ Phase 3（template）。

### 不做（明确划界）

- **不做 vcpkg 替代**。luban registry 永远是 fallback。试图重写 vcpkg port
  会让 luban 团队陷入永恒的 catch-up，且分裂 C++ 生态。
- **不做 build-from-source**。所有 lib / lib-rs 是 prebuilt binary 进 release。
  build-from-source 用户应当走 vcpkg port + custom triplet，那里已经成熟。
- **不做 mutable index**。每个版本号是 immutable；要"修 bug"就发新 patch
  版本（`X.Y.Z+luban1` patch label）。和 vcpkg 一样。
- **不做 "luban registry 装的库的依赖也走 luban registry" 递归**。每个 lib
  自己解决 transitive dep（要么 vendor，要么显式声明 vcpkg-equivalent dep）。
  避免 luban registry 自己变成一棵 dep 树。

## Consequences

**Pros**：
- 解决 OQ-7 三个真实痛点（vcpkg 缺货、Rust 明星库、模板生态）
- 跟 vcpkg "互补不替代"——降低生态分裂风险
- Rust FFI 包装工艺集中在 luban-build/* repo，每库 maintain 1 处而不是 N 处
- toolchain 迁过来后 manifests_seed/ 这个临时模型可以正式退役

**Cons / 风险**：
- **维护负担**。每个 lib-rs 包要跟上游 Rust crate 的 release 节奏；reqwest 主版本变了
  cxx-bridge 配置可能要改。M4 进入前需要明确"哪些库 maintain，哪些不"——草案候选
  集（≤ 10 个）：reqwest / tokio / serde-json-cpp / regex / unicode-segmentation /
  uuid / chrono / sha2-cpp / base64-cpp / log。
- **schema 锁定风险**。luban.json schema=1 一旦发布就不能动；要加字段必须 schema=2
  + reader fallback。**做法**：v1 schema 用 `additionalProperties: true` 留活路。
- **catalog 性能**。包多了之后 catalog.json 会变大；客户端要 lazy load index/<pkg>/
  而不是预拉全表。M4.1 起：客户端按需 fetch。
- **trust model**。luban registry 是 luban 团队 + 社区贡献；用户必须信任 sha256 +
  GitHub Releases 不被篡改。后续 ADR：introduce sigstore / cosign 签名。
- **OQ-2 toolchain pin 与 [luban-deps] 的关系**：toolchain 迁到 luban registry
  后 [toolchain] 段（OQ-2）应不应该并入 [luban-deps]？倾向"不"——toolchain pin
  是 dev env 行为，[luban-deps] 是项目运行时依赖，分开更清楚。

## Alternatives considered

- **直接给 vcpkg 提 PR**：解 1（vcpkg 缺货）但解不了 2（Rust crate）+ 3（模板）；且
  vcpkg PR 节奏远慢于我们想要的。
- **依靠 conan**：增加生态复杂度（用户已经要懂 vcpkg + cmake + luban），让 luban 包
  conan 在自己 inside 也违反 ADR-0001（多套包管理器堆叠）。
- **重新发明 vcpkg**：违反 ADR-0001 的"互补不替代"准则；维护负担爆表。
- **不做**：vcpkg 缺货 + Rust 明星库的痛点持续存在，OQ-7 长期挂着。

## Open questions（M4 启动前要拍板）

1. luban registry 的 namespace 规则：要不要 `org/pkg` 风格（npm）还是平铺（cargo）？
   倾向**平铺**（少打字、跟 vcpkg 习惯一致）。
2. `luban add jq --source=luban-registry` 还是 `luban add @luban-registry/jq`？
   倾向**前者**（flag 比 namespace 干净；M5 加 mirror 时 `--source=mirror.example.com`）。
3. lib-rs 包的 cxx 版本是不是要嵌进 package version？例如 `0.11.27-cxx0`？
   倾向**是**——schema 升级时旧 cxx 出来的 binary 不兼容新 cxx；版本字段嵌出来用户
   能看出"这是哪一代包装工艺产出的"。
4. luban registry 自身的 `luban-registry/lockfile.json` 在用户项目里要不要 commit？
   类比 cargo `Cargo.lock` —— **要 commit**，给用户 reproducible build；luban gen
   时一并生成。
5. M4 候选 lib-rs 集合的 final-cut（≤ 10 个）：reqwest / tokio 必装，剩下 ?

## 下一步

不在本 ADR 落实施代码。按 phase：

- **M4 Phase 1（写完 ADR-0006 POSIX 之后）**：建 `luban-registry` 仓库；schema=1
  落地；toolchain 类（cmake/ninja/llvm-mingw 等）从 `manifests_seed/` 迁过去
  并保留 fallback 1 个 release 周期。改 `commands/setup.cpp`、`commands/add.cpp`
  支持 `--source=luban-registry`。
- **M4 Phase 2**：建 `luban-build/libjq`、`luban-build/reqwest-cpp` 两个 build
  repo + Releases pipeline。验证 lib + lib-rs 路径在用户项目里是 build-and-link
  能跑通的。
- **M4 Phase 3**：template 类。`luban new --template=grpc-server` 从 luban-registry
  下模板。**先把 luban 自己的 templates/ 迁过去**，作为吃自己狗粮验证。
- 撰 ADR-0007（lib-rs 工艺细节）+ ADR-0008（template schema）作为 M4 Phase 2/3
  的 entry doc。
