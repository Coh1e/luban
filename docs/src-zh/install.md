# 安装

luban 是一个静态链接的 Windows 单二进制（~3 MB）。无 installer，无 PATH 仪式，无需 admin。

## 前置

- Windows 10（1809+）或 Windows 11，目前仅 x64
- 网络（`luban setup` 要拉 ~250 MB 工具链）

就这。不需要 Python / Visual Studio / Chocolatey / Scoop。

## 安装方式

### 方式 A —— 直接下二进制

1. 从 [GitHub Releases](https://github.com/Coh1e/luban/releases/latest) 下载 `luban.exe` 和 `luban-shim.exe`
2. 放到任意位置，例如 `%USERPROFILE%\bin\luban.exe`
3. 接下来要么直接用全路径调，要么放到 PATH（方式 C 自动做这事）

### 方式 B —— 从源码编译

如果你已经有 C++ 工具链（MSVC / MinGW / Clang）+ cmake + ninja：

```bat
git clone https://github.com/Coh1e/luban.git
cd luban
cmake --preset release
cmake --build --preset release
:: build/release/luban.exe + luban-shim.exe
```

### 方式 C —— 单文件自举（推荐）

```bat
luban.exe setup            :: 装 LLVM-MinGW 22 + cmake 4.3 + ninja 1.13 + mingit 2.54 + vcpkg 2026.03
luban.exe env --user       :: rustup 风格 HKCU PATH 注入
```

之后**任何新 shell** 都能直接 cmake / clang / clangd / ninja / git / vcpkg。再也不用 source activate 脚本。

> **小贴士**：`luban env --user` 之后，把 `luban.exe` 自身也复制到 `<data>\bin\`，这样它也在 PATH 上：
>
> ```bat
> copy luban.exe %LOCALAPPDATA%\luban\bin\luban.exe
> copy luban-shim.exe %LOCALAPPDATA%\luban\bin\luban-shim.exe
> ```

## 验证

```bat
luban doctor
```

期望看到：

```text
→ Canonical homes
  ✓ data    C:\Users\you\.local\share\luban
  ...
→ Installed components
  ✓ cmake                        4.3.2
  ✓ llvm-mingw                   20260421
  ✓ mingit                       2.54.0
  ✓ ninja                        1.13.2
  ✓ vcpkg                        2026.03.18
→ Tools on PATH
  ✓ clang          C:\Users\you\.local\share\luban\bin\clang.exe
  ...
```

如果某个工具显示 `(not found)`——**先开个全新终端**，env 改动只对新进程生效。

## 装在哪

```
%LOCALAPPDATA%\luban\
  toolchains\
    cmake-4.3.2-x86_64\          # cmake.exe + 共享数据
    llvm-mingw-20260421-x86_64\  # clang/clang++/clangd/lld + sysroot
    ninja-1.13.2-x86_64\
    mingit-2.54.0-x86_64\
    vcpkg-2026.03.18-x86_64\
  bin\                            # rustup 风格 shim 目录（env --user 后在 PATH 上）
    cmake.cmd / cmake.ps1 / cmake / cmake.exe ...
  env\                            # 生成的 activate 脚本
%USERPROFILE%\.cache\luban\downloads\
%USERPROFILE%\.local\state\luban\
%USERPROFILE%\.config\luban\
```

详细 XDG 解析见 [English: paths reference](https://luban.coh1e.com/reference/paths.html)。

## 升级

```bat
luban self update          :: 自动从 GH releases 拉最新版，校 SHA256，原子换
```

## 卸载

```bat
luban self uninstall --yes              :: 完全清理（含工具链）
luban self uninstall --yes --keep-data  :: 留工具链，仅清 luban 自己 + PATH 注入
```

luban 从不写 `<data>` / `<cache>` / `<state>` / `<config>` / `HKCU\Environment` 之外的位置。`self uninstall` 是逆向操作。
