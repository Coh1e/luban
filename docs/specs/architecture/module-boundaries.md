# 模块边界

luban 源码 ~6.5k 行 C++23，按 **职责单一** 拆分。每个模块的对外 API 都尽量收敛
到几个公共函数。新增功能时**优先扩展现有模块**，避免新增模块除非职责确属新增。

## 顶层

| 模块 | 文件 | 对外 API（关键） | 谁调用 |
|---|---|---|---|
| `cli` | `src/cli.{hpp,cpp}` | `Subcommand`、`ParsedArgs`、`forward_rest` | `main.cpp` 注册所有 verb |
| `paths` | `src/paths.cpp` | `data_dir()` / `cache_dir()` / `state_dir()` / `config_dir()` / `toolchain_dir(rel)` | 几乎所有模块 |
| `proc` | `src/proc.cpp` | `proc::run(cmd, cwd, env_overrides)` | 凡是 spawn 子进程的 verb |
| `download` | `src/download.cpp` | `download::download(url, dest, opts)` 含流式 SHA256 | `component`、`bucket_sync` |
| `archive` | `src/archive.cpp` | `archive::extract(zip, dest)`，含 zip-slip 防护 | `component` |
| `hash` | `src/hash.cpp` | `verify_file`、`HashSpec parse` | `download`、`component` |
| `env_snapshot` | `src/env_snapshot.cpp` | `apply_to(env)` 注入 PATH + VCPKG_* + EM_CONFIG (+ MSVC env if captured) | `proc::run` 的调用者 |
| `perception` | `src/perception.cpp` | `host::info() → JSON`：OS / CPU / RAM / SIMD / tools / XDG | `describe --host`、AGENTS.md 渲染 |
| `msvc_env` | `src/msvc_env.cpp` | vswhere 探测 + vcvarsall capture → `<state>/msvc-env.json` | `env_snapshot`、`commands/env --msvc-init` |

## 工具链 / 系统层

| 模块 | 文件 | 职责 |
|---|---|---|
| `manifests_seed/` | 数据 | 默认 selection + 各组件 manifest（含 cmake/ninja/llvm-mingw/mingit/vcpkg/node/emscripten/doxygen overlay） |
| `manifest_source` | `src/manifest_source.cpp` | overlay → seed 兜底查找；**无网络**（v0.2 取代了 bucket_sync） |
| `scoop_manifest` | `src/scoop_manifest.cpp` | 解析并安全过滤（拒绝 installer-script、msi/nsis），含 `luban_mirrors` 扩展字段 |
| `component` | `src/component.cpp` | 完整安装管线：下载 → 校验 → 解压 → 特例 bootstrap (vcpkg / emscripten) → 写 shim → 入 registry |
| `registry` | `src/registry.cpp` | `installed.json` schema=1 读写 + `resolve_alias` |
| `shim` | `src/shim.cpp` | `.cmd` shim 写出 + `is_managed` 碰撞检测；v0.2 起不再写 .ps1 / .sh |
| `shim_exe` | `src/shim_exe/main.cpp` | 独立二进制 `luban-shim.exe`，硬链接成各 alias |
| `win_path` | `src/win_path.cpp` | HKCU PATH / 环境变量读写、`WM_SETTINGCHANGE` 广播 |

## 项目 / 工程层

| 模块 | 文件 | 职责 |
|---|---|---|
| `vcpkg_manifest` | `src/vcpkg_manifest.cpp` | 安全编辑 `vcpkg.json`（add/remove/save，原子写） |
| `lib_targets` | `src/lib_targets.cpp` | 表驱动：`port → find_package + targets[]`（125 条目；含 librdkafka/cppkafka） |
| `luban_cmake_gen` | `src/luban_cmake_gen.cpp` | 渲染并原子写 `luban.cmake`（v1 schema） |
| `luban_toml` | `src/luban_toml.cpp` | 解析 `luban.toml` 偏好；`cpp` 字段校验 ∈ {17,20,23} |
| `commands/new_project` | 同名 | 模板展开（`{{name}}` 在内容和路径都生效） |
| `commands/build_project` | 同名 | 自动选 preset + `cmake/emcmake` |
| `commands/doc` | 同名 | `luban doc` — doxygen 包装（cargo doc 风格） |
| `commands/{add,target_cmd,which_search,doctor,describe,specs,self,setup,env,shim,...}` | 各自 | 见 `../interfaces/api-overview.md` |

## 不变量（不要打破）

1. **不引入新第三方依赖**，除非是 single-header 且 vendor 到 `third_party/` + LICENSE。
2. **不做路径 canonicalize**——`weakly_canonical()` 在 junction（`D:\dobby` → `C:\Users\Rust`）
   上会跨边，破坏 `relative_to()`。一律保留用户输入字面值。
3. **`luban.cmake` schema 稳定**——已经在野的项目里 git-tracked。变更必须 v2-section + 兼容读回。
4. **`vcpkg.json` 是项目依赖唯一真相**——不要在 `luban.toml` 加平行 `[deps]`。
5. **toolchain ≠ 项目库**——`luban add cmake` 必须被拒绝（见 `commands/add.cpp` system-tools 列表）。
6. **HKCU only**，永不写 HKLM。
7. **luban.exe 必须 static-linked**（`-static -static-libgcc -static-libstdc++`）。
