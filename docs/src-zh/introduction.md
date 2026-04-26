# Luban

> Windows-first 的 C++ 工具链管理器 + cmake/vcpkg 辅助前端。
> 单一静态链接二进制，零 UAC，XDG-first 目录布局。

## luban 解决什么问题

如果你在 Windows 上配过现代 C++ 工具链，你就知道墙在哪：

- 选 LLVM-MinGW 还是 MSVC 还是 mingw-w64
- 各自下 cmake / ninja / clangd / vcpkg
- 才发现 vcpkg 还要 git
- 学 cmake 的怪 variable / find_package / target_link_libraries
- 学 vcpkg.json schema、triplets、overlay ports
- 学 CMakePresets
- 才意识到 clangd 要 `compile_commands.json` 才有补全
- 然后所有这些**都不在 PATH 上**——每个新 shell 还得 source 个 activate

每件事是半天的 yak-shaving。整套堆起来要好几天，每台新机器要重来一遍。

**luban 把这堆全活变成两条命令：**

```bat
luban setup          # 一次性，装 LLVM-MinGW + cmake + ninja + mingit + vcpkg
luban env --user     # 一次性，把所有工具注入 user PATH（rustup 风格）
```

之后开任何新终端，cmake / clang++ / ninja / clangd 直接可用。每个项目里：

```bat
luban new app foo    # 脚手架；自动 build；clangd 立即可用
cd foo
luban add fmt        # 一行加依赖
luban build          # cmake 通过 vcpkg manifest mode 自动取 fmt
```

**80% 的场景下，你永远不打开 `CMakeLists.txt`。**

## luban 不是什么

- **不是构建系统。** cmake + ninja 仍然干活；luban 只生成 cmake 胶水。
- **不是包管理器。** vcpkg 仍然解析 + 构建 C++ 包；luban 替你编辑 `vcpkg.json`。
- **不是替代品。** 你 `git clone` 一个 luban 项目到一台没装 luban 的机器
  上，只要 cmake + vcpkg 在，`cmake --preset default` 仍然能 build——
  luban 生成的 `luban.cmake` 进 git 跟随项目。

## 设计哲学

| 原则 | 含义 |
|---|---|
| **辅助而非主导** | cmake / vcpkg / ninja 仍是项目主角。luban 写一个标准 cmake module（`luban.cmake`）让用户 `include()`，删一行就退出。 |
| **不发明新 manifest** | 依赖在 `vcpkg.json`（vcpkg 自己的 schema）里。`luban.toml` 是**可选的**，只放项目级偏好（warning 等级、sanitizers）。 |
| **`luban.cmake` 进 git** | 项目可重现：clone 到没装 luban 的机器仍能 build。 |
| **XDG-first 目录** | `~/.local/share/luban/`（data）、`~/.cache/luban/`（cache）、`~/.local/state/luban/`（state）、`~/.config/luban/`（config）。`XDG_*` 环境变量优先。 |
| **零 UAC** | 所有 luban-managed 文件落在用户可写目录。绝不弹 admin 提示。 |
| **单静态二进制** | 一个 `luban.exe` 包一切。不依赖 Python / MSI / Visual Studio。 |

完整 8 条架构不变量见 [设计总览](./architecture/design.md)。

## 接下来

- [安装](./install.md) — 把 `luban.exe` 装到机器上
- [快速上手](./quickstart.md) — 5 条命令到 "hello, fmt"
- [日常使用循环](./daily-loop.md) — 一周 luban 用法长什么样
- [设计总览](./architecture/design.md) — luban 为什么长这样
