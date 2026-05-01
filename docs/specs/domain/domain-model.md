# 领域模型

luban 的核心实体围绕 "工具链" 和 "项目" 两类资源展开。下表是 v1 schema 快照。

## 系统侧（toolchain / component）

### Component

由 manifest 描述、由 `luban setup` 安装的工具链单元。

| 属性 | 来源 | 含义 |
|---|---|---|
| `name` | manifest 文件名 | 唯一标识，如 `llvm-mingw`、`cmake`、`vcpkg` |
| `version` | manifest `version` | 安装的版本号 |
| `arch` | 选择 / manifest | 构架（默认 x64） |
| `url` | manifest `url` | 下载源（GitHub Release 等） |
| `hash` | manifest `hash` | 形如 `sha256:528ff87…` |
| `extract_dir` | manifest，可选 | zip 内子目录定位 |
| `bin` | manifest `bin: [[alias, rel_path], …]` | 暴露给 PATH 的可执行映射 |
| `depends` | manifest，可选 | 传递依赖（拓扑排序前置） |
| `toolchain_dir` | 安装时计算 | `<data>/toolchains/<name>-<ver>-<arch>` 相对路径 |
| `installed_at` | 安装时 | ISO-8601 时间戳 |

### Alias

可执行别名。一个 alias 解析为一个具体 exe + 一组前置参数。

| 属性 | 含义 |
|---|---|
| `alias` | 用户可调用的命令名（在 `<data>/bin/` 下） |
| `exe` | 解析后的绝对路径 |
| `prefix_args` | 形如 `["-S", ".", "-B", "build"]`（多数为空） |

`registry::resolve_alias(name)` 是唯一查找入口。

### Shim

把 alias 物化到磁盘的产物：

- `<alias>.cmd`、`<alias>.ps1`、`<alias>.sh`（text shim）
- `<alias>.exe` —— `luban-shim.exe` 的硬链接，运行时读 `.shim-table.json` 反查

text shim 给交互式 shell 用；exe shim 给 cmake 编译器探测之类必须看到 `.exe` 文件的场景用。

## 项目侧（project / target）

### Project

| 属性 | 来源 | 含义 |
|---|---|---|
| `root` | `find_project_root()` 向上找含 `vcpkg.json` 的目录 | 项目根 |
| `vcpkg_json` | `<root>/vcpkg.json` | 依赖来源（git-tracked） |
| `luban_toml` | `<root>/luban.toml`（可选） | 项目偏好 |
| `luban_cmake` | `<root>/luban.cmake`（git-tracked） | 由 luban 重写的 cmake 模块 |
| `presets` | `<root>/CMakePresets.json` | 用户拥有 |
| `targets` | `read_targets_from_cmake()` / fallback | LUBAN_TARGETS 列表 |

### Target

`add_executable` / `add_library` 一对一对应。脚手架创建时落在 `src/<name>/`。

| 属性 | 含义 |
|---|---|
| `name` | target 名 |
| `kind` | `exe` / `lib` |
| `subdir` | `src/<name>` |

### Dependency

| 属性 | 来源 | 含义 |
|---|---|---|
| `port` | `vcpkg.json` `dependencies[].name` | vcpkg port 名 |
| `version_ge` | `vcpkg.json` `version>=` | 可选下界 |
| `find_package` | `lib_targets.cpp` 表查 | cmake 包名（多对一） |
| `targets[]` | `lib_targets.cpp` 表查 | cmake 链接目标，如 `fmt::fmt` |

## 状态文件

| 文件 | 路径 | schema | 谁写 | 谁读 |
|---|---|---|---|---|
| `installed.json` | `<state>/` | `1` | `component::install` | `registry::*`、`env_snapshot` |
| `selection.json` | `<config>/`（默认从 seed 复制） | n/a | 用户 / `setup` | `setup` |
| `vcpkg.json` | `<project>/` | vcpkg 自定义 | `vcpkg_manifest::*`、`luban new` | vcpkg / luban |
| `luban.toml` | `<project>/`（可选） | luban v1 | 用户 | `luban_cmake_gen` |
| `luban.cmake` | `<project>/` | v1（自管） | `luban_cmake_gen::regenerate_in_project` | cmake |
