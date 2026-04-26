# Quickstart

From a fresh Windows 11 machine to a hello-world C++ project linked against a vcpkg library, in five commands.

## Prerequisites

- Windows 10/11 x64
- `luban.exe` somewhere reachable

## The five commands

```bat
luban setup            :: installs the toolchain (~250 MB, ~3 minutes)
luban env --user       :: registers tools on user PATH (one-time)

:: --- open a fresh terminal here so PATH picks up changes ---

luban new app hello    :: scaffolds + auto-builds; clangd ready
cd hello
luban add fmt          :: edits vcpkg.json + luban.cmake
luban build            :: cmake auto-fetches fmt via vcpkg, builds
build\default\src\hello\hello.exe
```

That's it. You should see:

```text
hello from hello!
```

Now edit `src/hello/main.cpp` to use `fmt`:

```cpp
#include <fmt/core.h>
#include <fmt/color.h>

int main() {
    fmt::print(fg(fmt::color::cyan), "hello from luban via vcpkg-installed fmt!\n");
    return 0;
}
```

Build again:

```bat
luban build
build\default\src\hello\hello.exe
```

## Open in your editor

Open the project in **Neovim** or **VS Code**. Both auto-detect:

- `compile_commands.json` (in project root after `luban build`) → clangd reads it directly
- `clangd.exe` is on PATH → no LSP config needed

If clangd is correctly attached, you should see autocomplete on `fmt::` and jump-to-definition into the vcpkg-installed fmt headers.

## What just happened

```
hello/
├── CMakeLists.txt          ← user-owned, 4 lines: project + include + register_targets
├── luban.cmake             ← luban-generated; find_package(fmt) + target_link_libraries
├── vcpkg.json              ← {"name":"hello", "version":"0.1.0", "dependencies":["fmt"]}
├── vcpkg-configuration.json ← baseline pinned to a specific vcpkg commit
├── CMakePresets.json       ← Ninja generator + vcpkg toolchain file (when VCPKG_ROOT set)
├── compile_commands.json   ← copied from build/ for clangd
├── .gitignore
├── .clang-format
├── .clang-tidy
├── .vscode/
└── src/
    └── hello/
        ├── CMakeLists.txt   ← user-owned, 2 lines
        └── main.cpp
```

You never opened `CMakeLists.txt`. You never wrote a `find_package` call. You never read the vcpkg manifest mode docs. Luban did all of that.

## Where to next

- [The Daily Driver Loop](./daily-loop.md) — what `luban` use looks like every day
- [Workflows → Multi-target project](./workflows/multi-target.md) — when one binary is not enough
- [Commands → `luban add`](./commands/add.md) — version constraints, feature selection
- [Reference → `luban.cmake`](./reference/luban-cmake.md) — what luban actually generates and why
