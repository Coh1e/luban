# Installation

Luban is a single static-linked Windows executable (~31 MB). No installer, no PATH dance, no admin rights required.

## Prerequisites

- Windows 10 (1809+) or Windows 11 — both `x64` only for now
- A working network connection (for `luban setup` to fetch toolchains; ~250 MB total download)

That's it. No Python, no Visual Studio, no Chocolatey/Scoop, nothing pre-installed.

## Pick your install style

### Option A — drop the binary anywhere

1. Download `luban.exe` from the [GitHub Releases](https://github.com/your-org/luban/releases) page.
2. Save it somewhere you'll remember — e.g. `%USERPROFILE%\bin\luban.exe`.
3. From the next step you can run it via full path or have it on PATH (Option C below).

### Option B — build from source

If you already have a C++ toolchain (MSVC, MinGW, Clang) with cmake + ninja:

```bat
git clone https://github.com/your-org/luban.git
cd luban
cmake --preset default
cmake --build --preset default
:: build/default/luban.exe
```

You can then use this freshly-built `luban.exe` to install Luban-managed toolchains.

### Option C — bootstrap fully from a single binary

```bat
luban.exe setup            :: installs LLVM-MinGW 22, cmake 4.3, ninja 1.13, mingit 2.54, vcpkg 2026.03
luban.exe env --user       :: rustup-style HKCU PATH integration (one-time)
```

After this, *any new shell* has cmake / clang / clangd / ninja / git / vcpkg on PATH. You'll never need to source an activate script again.

> **Tip:** After `luban env --user`, copy `luban.exe` itself into `<data>\bin\` so it's also on PATH:
>
> ```bat
> copy luban.exe %LOCALAPPDATA%\luban\bin\luban.exe
> ```

## Verify

```bat
luban doctor
```

Expected output:

```text
→ Canonical homes
  ✓ data    C:\Users\you\.local\share\luban
  ✓ cache   C:\Users\you\.cache\luban
  ...
→ Installed components
  ✓ cmake                        4.3.2
  ✓ llvm-mingw                   20260421
  ✓ mingit                       2.54.0
  ✓ ninja                        1.13.2
  ✓ vcpkg                        2026.03.18
→ Tools on PATH
  ✓ clang          C:\Users\you\.local\share\luban\bin\clang.cmd
  ✓ cmake          ...
  ...
```

If any tool says `(not found)` after `luban env --user`, **open a fresh terminal window** — the change only takes effect in newly-spawned shells, not the one that ran the command.

## What's installed and where

```
%LOCALAPPDATA%\luban\
  toolchains\
    cmake-4.3.2-x86_64\          # cmake.exe + share/cmake-4.3
    llvm-mingw-20260421-x86_64\  # clang/clang++/clangd/lld/libc++ + sysroot
    ninja-1.13.2-x86_64\
    mingit-2.54.0-x86_64\
    vcpkg-2026.03.18-x86_64\     # ports tree + vcpkg.exe
  bin\                            # rustup-style shim dir (on PATH after env --user)
    cmake.cmd / cmake.ps1 / cmake     # 280+ alias shims
  env\                            # generated activate scripts
    activate.cmd / .ps1 / .sh
%USERPROFILE%\.cache\luban\downloads\    # archive cache
%USERPROFILE%\.local\state\luban\        # installed.json (component registry)
%USERPROFILE%\.config\luban\             # selection.json (which components to install)
```

See [XDG-first directory layout](./reference/paths.md) for the full spec.

## Uninstall

```bat
luban env --unset-user                          :: remove from HKCU PATH/env
rmdir /S /Q %LOCALAPPDATA%\luban                :: ~250 MB of toolchains gone
rmdir /S /Q %USERPROFILE%\.cache\luban
rmdir /S /Q %USERPROFILE%\.local\state\luban
rmdir /S /Q %USERPROFILE%\.config\luban
del luban.exe                                   :: wherever you put it
```

Luban never writes outside these directories. There is no Windows Registry persistence beyond HKCU PATH/env (cleaned up by `luban env --unset-user`).
