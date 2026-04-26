# 设计总览

一页讲清：**luban 为什么长这样？**

如果你只读一篇架构文档，读这篇。其他架构文章（`philosophy` `two-tier-deps` `why-cmake-module` `roadmap`）放大讲单个话题——这篇是综合。

## luban 解决的问题

Windows 上的现代 C++ 项目要用户**自己拼装**：

1. 工具链（LLVM-MinGW / MSVC / mingw-w64）
2. cmake —— 项目级 meta-build
3. ninja —— 实际构建器
4. clangd —— 编辑器 LSP
5. vcpkg —— 包管理
6. git —— vcpkg 要它，自己也用
7. 一堆胶水：`compile_commands.json` / `find_package` / `target_link_libraries` /
   triplets / `CMakePresets` / `CMAKE_TOOLCHAIN_FILE` / `vcpkg-configuration.json`
   baseline 锁定 / `target_include_directories(... PUBLIC)` / 等等

每件事是半天 yak-shaving。整套堆起来要好几天，每台新机器要重来。

luban 的工作：把这些变成**两条一次性命令** + 一个**很小的项目内循环**：

```bat
luban setup                       :: ~3 min，~250 MB 工具链
luban env --user                  :: 注 user PATH（rustup 风格）
:: 开新终端

luban new app foo
cd foo
luban add fmt                     :: 编辑 vcpkg.json + luban.cmake
luban build                       :: vcpkg 在 cmake configure 期间自动拉 fmt
```

用户**全程不打开** `CMakeLists.txt`，**不写** `find_package`，**不读** vcpkg manifest mode 文档。

## luban 不是

- **不是构建系统。** cmake + ninja 仍然干构建。
- **不是包管理器。** vcpkg 仍然解析 + 构建 C++ 包。
- **不是 fork / 替换。** luban-managed 项目仍然是**标准 cmake 项目**——
  clone 到没装 luban 的机器，`cmake --preset default && cmake --build --preset
  default` 照样能 build。

luban 是**带主见的胶水**：把对的工具用对的方式粘起来，给初学者**默认就走在轨道上**的体验。

## 8 条架构不变量

锁死的设计决策。打破任何一条都让 luban 的根本契约失效：

### 1. cmake 永远是项目主角

luban **不发明 IR**。luban **不发明新 manifest 格式**取代 `CMakeLists.txt`。
luban 写一个**标准 cmake module**（`luban.cmake`），用户代码 `include()` 即用。
删掉一行 luban 就退出。

### 2. `luban.cmake` 进 git

"辅助"二字的整个意义就是项目得在没有 luban 时仍能 build。`luban.cmake`
是普通 cmake module，进 git 跟随项目，永远可重现。

### 3. 不发明新 manifest

项目依赖在 `vcpkg.json`（vcpkg 自己的 schema）里。`luban add` 编辑它。**没有**
平行的 `[deps]` 字段在 `luban.toml` 里——那会造成双真相。

`luban.toml` 仅作为**可选项目偏好**存在（warning 等级、sanitizers、默认 preset、triplet）——
那些不适合放 vcpkg.json 的零碎东西。

### 4. `luban.toml` 是可选的

很多 luban 项目根本没有 `luban.toml`。只在用户**真有**偏好时才出现。默认值 cover 80% 场景。

### 5. XDG-first 路径，Windows 上也是

luban 尊重 `XDG_DATA_HOME` / `XDG_CACHE_HOME` / `XDG_STATE_HOME` /
`XDG_CONFIG_HOME` 和 `LUBAN_PREFIX` 总开关变量，**优先**于 `%LOCALAPPDATA%` /
`%APPDATA%` fallback。

让 container / CI / 多用户场景一行配置搞定，Linux port 时无惊喜。

### 6. 零 UAC

每个 luban-managed 文件落在用户可写目录。luban 永不写 `Program Files`、
永不碰 `HKLM`、永不弹 admin。`luban env --user` 只写 `HKCU`（当前用户）。

### 7. 单静态链接二进制

`luban.exe` 一个文件：release ~3 MB / debug ~32 MB（含 vendored miniz / json /
toml++ 单 header 三件）。无配套 DLL、无 Python、无 MSI。U 盘装得下、新 VM 拷过去就跑。

### 8. 二层依赖模型

| 层 | 内容 | 落点 | 谁管 |
|---|---|---|---|
| **系统层** | cmake / ninja / clang / clangd / lld / git / vcpkg | `<data>/toolchains/` | `luban setup` |
| **项目层** | fmt / spdlog / boost / catch2 | `<project>/vcpkg_installed/<triplet>/` | `luban add`（编辑 vcpkg.json） |

混淆是被拒绝的。`luban add cmake` 会报错，引导到 `luban setup`。系统工具
machine-wide 锁版本，项目库每项目独立 baseline。

## 为什么是这个形态（拒绝的备选）

### 备选 A：luban 自己的 DSL → lower 到 cmake

xmake / meson / build2 / premake 都走这条路。用户写 `xmake.lua` 或
`meson.build`，工具生成实际 build 文件。

**为什么我们说 no**：C++ 有几十年的 cmake 习惯。custom commands、
target-specific flags、conditional features——每个真实项目都会撞到 DSL
没覆盖的边界，**fall through 到"反正还是要懂 cmake"**。这时候 DSL 只是个
额外要学的东西，没真的把 cmake 藏住。更糟的是 DSL 与 cmake 的接缝**很敌意**
（eject 仪式、单向转换）。

### 备选 B：在 `CMakeLists.txt` 里用标记块原地编辑

像 code generator 插入 `# >>> BEGIN luban` / `# <<< END luban` 段。luban
拥有那些块；用户拥有其余。

**为什么我们说 no**：原地编辑很脆。用户在标记内手改的东西被覆盖。**靠近**
标记的改动跟重生成 race。边界处永远是混乱源。

`luban.cmake` 干净解决这事：luban 拥有**整个文件**，用户 `include()` 它，
边界是硬切。

### 备选 C：顶层一个统一 manifest

`luban.toml` 包一切——deps / scripts / profiles / workspace / toolchain pin。
Cargo 风格。

**为什么我们说 no**：vcpkg 的 `vcpkg.json` 已经存在，是 C++ 事实 manifest。
发明并行的 `[deps]` 段意味着维持双真相，两边永远 drift。让 vcpkg 拥有它的
schema、luban 通过 `luban add` 编辑它就好。

### 备选 D：luban-init.exe 自举安装器

rustup 风格：一个小 installer 下载 luban + 跑 setup。

**为什么我们说 no**：luban.exe **本来就是**单文件下载。用户从 GH Releases
`curl -O luban.exe` 直接跑就好。bootstrapper 是无价值的中间层。改成
`luban self update` 和 `luban self uninstall` 把生命周期闭环放进同一个二进制，
uv 风格。

## 我们接受的 trade-off

- **Windows-first**。Linux/macOS port 是 M3+ 工作；短期失多平台卫生，长期
  让我们在最需要帮助的平台（Windows C++ tooling）上快速迭代。
- **只 vendor 单 header**。不消费系统 `find_package(zlib)` 类。多 ~10MB 二进制
  换"一个文件、随处能跑"。
- **`.cmake` module 模式**假设用户会 `include(luban.cmake)`。不 include 就没有
  自动 find_package 魔法——掉进普通 cmake。这是设计，不是 bug。
- **curated pkg→target 表**。`luban add` 内置 ~50 流行库的映射。未知 port
  写 `find_package(<port>)` 但不自动 link，用户自己填 target 名。长尾未来通过
  scrape vcpkg 的 `usage` 文件解决。

## 操作模型

luban 作为**三层**运作，每层都有稳定接口：

```
┌─ 用户 ─────────────────────────────────────────────────┐
│   luban CLI（16 verb，分 4 组）                         │
└──────────────────────────────────────────────────────┘
            │ 编辑 vcpkg.json / luban.toml
            │ 调 cmake / vcpkg
            ▼
┌─ luban-managed 产物 ──────────────────────────────────┐
│   luban.cmake     ← luban 写；进 git                   │
│   <data>/bin/     ← shim 目录；在 user PATH 上          │
│   <state>/installed.json   ← 组件 registry             │
└──────────────────────────────────────────────────────┘
            │ shell out to
            ▼
┌─ 外部工具 ────────────────────────────────────────────┐
│   cmake / ninja / clang / vcpkg / git                 │
│   （luban 下载 + 管理它们，但不侵入它们的 config       │
│    —— 它们仍然是"现成"的）                              │
└──────────────────────────────────────────────────────┘
```

中间那层是 luban "辅助"性的体现：薄、稳、可 git 跟踪的 spec，桥接用户意图与
标准工具。**抽掉中间层，下面那层仍然是能用的工具。**

## 接下来读

- [Design philosophy （英文版） →](https://luban.coh1e.com/architecture/philosophy.html)
- [Two-tier dependency model （英文版） →](https://luban.coh1e.com/architecture/two-tier-deps.html)
- [Why no IR, why luban.cmake （英文版） →](https://luban.coh1e.com/architecture/why-cmake-module.html)
- [Roadmap （英文版） →](https://luban.coh1e.com/architecture/roadmap.html)
- [快速上手](../quickstart.md) —— 5 命令一遍走
