# 快速上手

从全新 Windows 11 到一个链了 vcpkg 库的 hello-world C++ 项目，五条命令搞定。

## 前置

- Windows 10/11 x64
- `luban.exe` 在某处可达

## 五条命令

```bat
luban setup            :: 装工具链（~250 MB，~3 分钟）
luban env --user       :: 注 user PATH（一次性）

:: --- 此时关掉当前终端，开新终端 ---

luban new app hello    :: 脚手架 + 自动 build；clangd 即用
cd hello
luban add fmt          :: 编辑 vcpkg.json + 重生成 luban.cmake
luban build            :: cmake 通过 vcpkg 自动取 fmt + 编 + 链
build\default\src\hello\hello.exe
```

应该看到：

```text
hello from hello!
```

现在改 `src/hello/main.cpp` 用上 `fmt`：

```cpp
#include <fmt/core.h>
#include <fmt/color.h>

int main() {
    fmt::print(fg(fmt::color::cyan), "hello from luban via vcpkg-installed fmt!\n");
    return 0;
}
```

再 build 跑：

```bat
luban build
build\default\src\hello\hello.exe
```

## 在编辑器里打开

用 **Neovim** 或 **VS Code** 打开。两边都自动检测：

- 项目根的 `compile_commands.json`（`luban build` 后产生）→ clangd 直读
- PATH 上的 `clangd.exe` → LSP 立即就位

clangd 启动后，对 `fmt::` 应有自动补全 + 跳定义到 vcpkg 装的 fmt 头。

## 刚刚发生了什么

```
hello/
├── CMakeLists.txt          ← 用户拥有，4 行：project + include + register_targets
├── luban.cmake             ← luban 生成；find_package(fmt) + target_link_libraries
├── vcpkg.json              ← {"name":"hello", "version":"0.1.0", "dependencies":["fmt"]}
├── vcpkg-configuration.json ← baseline 锁定到具体 vcpkg commit（可重现性保证）
├── CMakePresets.json       ← Ninja generator + vcpkg toolchain（VCPKG_ROOT 在时启用）
├── compile_commands.json   ← 从 build/ 复制过来给 clangd
├── .gitignore
├── .clang-format
├── .clang-tidy
├── .vscode/
└── src/
    └── hello/
        ├── CMakeLists.txt   ← 用户拥有，2 行
        └── main.cpp
```

你**没打开** `CMakeLists.txt`。**没写** `find_package` 调用。**没读** vcpkg manifest mode 文档。luban 全替你做了。

## 接下来

- [日常使用循环](./daily-loop.md) — 第二周长什么样
- [设计总览](./architecture/design.md) — luban 为什么长这样
- [English: full command reference →](https://luban.coh1e.com/commands/overview.html)
- [English: workflows →](https://luban.coh1e.com/workflows/first-run.html)
