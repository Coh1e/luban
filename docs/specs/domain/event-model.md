# 事件模型（lifecycle）

按 "操作动作" 梳理关键状态机与跨模块的因果链。每个事件都有：触发、写入、读出、副作用。

## E1. 工具链安装（setup）

```
luban setup [--with X]
   └─ commands/setup.cpp::run_setup
        ├─ 读 selection.json（默认从 seed 复制到 <config>）
        ├─ expand_depends() ── 拉 manifest，DFS post-order 排序
        └─ for each component:
             component::install
               ├─ download ── WinHTTP 流式 + 流式 SHA256
               ├─ verify   ── hash::verify_file（cache 命中也走同路径）
               ├─ extract  ── miniz, zip-slip 防护
               ├─ post-extract bootstrap：
               │    vcpkg     → bootstrap-vcpkg.bat
               │    emscripten→ 写 .emscripten 配置（指 LLVM_ROOT/NODE_JS 等）
               ├─ shim::write_shim 写出 .cmd/.ps1/.sh + 硬链接 .exe
               └─ registry::save_installed 写 installed.json (schema=1)
```

**幂等性**：同 `name+version+arch` 已存在则直接复用，跳过下载。

## E2. 注册到当前用户（env --user）

```
luban env --user
   └─ commands/env.cpp
        ├─ win_path::add_to_user_path(<data>/bin)
        │    ├─ 读 HKCU\Environment\PATH（注意 REG_SZ vs REG_EXPAND_SZ）
        │    ├─ 去重（不区分大小写） + prepend
        │    └─ 广播 WM_SETTINGCHANGE
        └─ for each (key,val) in env_dict():
             win_path::set_user_env(key, val)   # LUBAN_DATA/CACHE/STATE/CONFIG
        └─ if vcpkg installed → 设 VCPKG_ROOT
```

## E3. 项目创建（new）

```
luban new app foo [--target=wasm]
   └─ commands/new_project.cpp::run_new
        ├─ find_template_root("app" 或 "wasm-app")
        ├─ materialize(template_root, project_root, {name=foo, ...})
        │    ├─ walk *.tmpl，剥扩展名
        │    ├─ {{name}} 在文件内容、目录名都做替换
        │    └─ 非 .tmpl 文件直接拷贝
        └─ 默认调 luban build（生成 compile_commands.json，验证可编）
```

**首次创建后**：`CMakeLists.txt`、`CMakePresets.json`、`vcpkg.json`、
`vcpkg-configuration.json`、源文件——**全部归用户所有**。luban 之后只重写 `luban.cmake`。

## E4. 加 / 删依赖（add / remove / sync）

```
luban add fmt [--version 10]
   └─ commands/add.cpp
        ├─ is_system_tool("fmt") → 否（"cmake" 等会在此报错）
        ├─ find_project_root()
        ├─ vcpkg_manifest::add(manifest, "fmt", "10")
        │    └─ save() 原子写 vcpkg.json
        └─ luban_cmake_gen::regenerate_in_project(root, targets)
             ├─ 读 luban.toml + vcpkg.json
             ├─ render() → v1 schema
             │    ├─ LUBAN_TARGETS 列表（来自 read_targets_from_cmake）
             │    ├─ find_package(fmt CONFIG REQUIRED) 块
             │    ├─ luban_apply(target) 函数
             │    └─ luban_register_targets() 函数
             └─ 原子写 luban.cmake
```

`luban sync` 是 `add/remove` 的纯重渲染（不改 vcpkg.json）。

## E5. 构建（build）

```
luban build
   └─ commands/build_project.cpp
        ├─ 选 preset：
        │    has wasm preset           → "wasm"
        │    else has deps in vcpkg.json → "default"
        │    else                       → "no-vcpkg"
        ├─ env_snapshot::apply_to(env) 注入 toolchain PATH + LUBAN_*
        ├─ proc::run(cmake/emcmake --preset <P>, env=...)
        ├─ proc::run(cmake --build --preset <P>, env=...)
        └─ 同步 build/<P>/compile_commands.json → 项目根
```

## E6. Alias 解析（任何调 alias 的场景）

```
当用户 / cmake / 子进程在 PATH 上找到 <data>/bin/<alias>.exe：
   luban-shim.exe（硬链接）
        ├─ GetModuleFileNameW → 自身路径 → 反推 alias
        ├─ 读 <data>/bin/.shim-table.json
        └─ CreateProcessW(exe, argv) + 等待 + 透传 exit code

文本 shim（.cmd/.ps1/.sh）走 shell 解释。
```

## E7. 自我管理（self update / uninstall）

- **update**：从 GitHub Releases 拉新 `luban.exe`，写到临时位置，原地替换。
- **uninstall**：清理 `<data>/<cache>/<state>/<config>`，撤销 HKCU PATH/env 修改。
