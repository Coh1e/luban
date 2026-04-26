# 日常使用循环

一次性配置（`luban setup` + `luban env --user`）做完后，一周 luban 用法是什么样。

## 周一：起新项目

```bat
luban new app weekly-experiment
cd weekly-experiment
nvim src/weekly-experiment/main.cpp     # clangd 立即自动补全
```

`luban new` 末尾自动跑 `luban build` 一次，`compile_commands.json` 已落盘，clangd 一启动就能吃。

## 周二：要个 JSON parser

```bat
luban add nlohmann-json
luban build              # vcpkg 拉它 + 编 + cache（首次 ~30s）
```

`luban add` 改了 `vcpkg.json` 和重写了 `luban.cmake`。**你没打开 `CMakeLists.txt`**。**没查 cmake target 名**（是 `nlohmann_json::nlohmann_json`，luban 内置映射表替你处理）。

## 周三：拆出一个库

`main.cpp` 越来越长。把 parsing 逻辑抽成静态 lib：

```bat
luban target add lib parser
```

得到 `src/parser/{parser.h, parser.cpp, CMakeLists.txt}`。改这三个文件。然后 `src/weekly-experiment/CMakeLists.txt` 加**一行**：

```cmake
target_link_libraries(weekly-experiment PRIVATE parser)
```

`luban build` —— 两个 target 全编、exe 链 lib。

## 周四：写测试

```bat
luban add catch2
luban target add exe test-parser
```

把 `test-parser` 接到 `parser` + `Catch2::Catch2WithMain`。剩下都是标准 cmake。

## 周五：同事 clone 你的项目

同事的机器有 cmake + ninja + vcpkg，但**没装 luban**：

```bat
git clone <你仓库>
cd weekly-experiment
cmake --preset default
cmake --build --preset default
```

直接 work。`luban.cmake` 进了 git，cmake 直 include；vcpkg 用同 baseline 拉同版本依赖。同事**根本不知道 luban 存在**。

## 每天改了什么

| 天 | luban 改的文件 | 你改的文件 |
|---|---|---|
| 周一 | `vcpkg.json` `luban.cmake`（初始化）| 无 |
| 周二 | `vcpkg.json` `luban.cmake` | `main.cpp` |
| 周三 | `luban.cmake`（加 LUBAN_TARGETS）+ 新建 `src/parser/*` | `main.cpp` / `src/weekly-experiment/CMakeLists.txt`（+1 行）/ `parser.{h,cpp}` |
| 周四 | `vcpkg.json` `luban.cmake` + 新建 `src/test-parser/*` | 各种 |
| 周五 | （无——只跑 cmake）| （无——只跑 cmake）|

你**整个周里**没打开根 `CMakeLists.txt` 一次。没写 `find_package` 调用。没读 vcpkg manifest mode 文档。

## 什么时候应该跳出 luban

luban 擅长 80% 场景。下面这些让你直接写 cmake，不要走 luban：

- **每文件特殊编译 flag**：写在 `src/<target>/CMakeLists.txt`，`luban_apply()` 之后
- **非 vcpkg 的依赖**：`include(luban.cmake)` 之后写自己的 `find_package(MyLib)`，luban 不会跟你抢
- **罕见 generator**（Make / VS / Xcode）：改 `CMakePresets.json`（用户拥有）
- **header-only lib target**：在 `src/<lib>/CMakeLists.txt` 把 `add_library(<name> STATIC ...)` 改成 `add_library(<name> INTERFACE)` 自行调整
- **非 vcpkg 来源的依赖**：跳过 `luban add`，自己写 `find_package` / FetchContent

退出方式：把根 `CMakeLists.txt` 里的 `include(luban.cmake)` 删掉，整个项目变成普通 cmake 工程。
