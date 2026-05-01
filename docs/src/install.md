# Installation

Luban is a single static-linked Windows executable (~31 MB). No installer, no PATH dance, no admin rights required.

## Prerequisites

- Windows 10 (1809+) or Windows 11 — both `x64` only for now
- A working network connection (for `luban setup` to fetch toolchains; ~250 MB total download)

That's it. No Python, no Visual Studio, no Chocolatey/Scoop, nothing pre-installed.

## Pick your install style

### Option A — one-line install (recommended)

```powershell
irm https://luban.coh1e.com/install.ps1 | iex
```

The installer drops `luban.exe` and `luban-shim.exe` into `~/.local/bin/`
(XDG-style, shared with uv / pipx / claude-code), verifies SHA256 against
the published `SHA256SUMS`, and prompts you to run `luban env --user` and
`luban setup` for a complete bootstrap.

Override the install dir with `$env:LUBAN_INSTALL_DIR` before piping.

### Option B — manual download

1. Download `luban.exe` and `luban-shim.exe` from the [GitHub Releases](https://github.com/Coh1e/luban/releases) page.
2. Drop both into `%USERPROFILE%\.local\bin\` (or anywhere on your PATH).
3. Run `luban env --user` to register `<data>/bin` (toolchain shim dir) on
   HKCU PATH, then `luban setup` to install the default toolchain.

### Option C — build from source

If you already have a C++ toolchain (MSVC, MinGW, Clang) with cmake + ninja:

```bat
git clone https://github.com/Coh1e/luban.git
cd luban
cmake --preset default
cmake --build --preset default
:: build/default/luban.exe
```

You can then use this freshly-built `luban.exe` to install Luban-managed toolchains.

### Bootstrap to a working environment

After Option A or B has dropped luban into `~/.local/bin/`:

```bat
luban setup                :: installs LLVM-MinGW, cmake, ninja, mingit, vcpkg
luban env --user           :: rustup-style HKCU PATH integration (one-time)
```

After this, *any new shell* has cmake / clang / clangd / ninja / git / vcpkg
on PATH. The luban.exe in `~/.local/bin/` is your control plane; toolchain
shims live in `<data>/bin/` (added to PATH by `env --user`, cargo style).

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
%USERPROFILE%\.local\bin\               # XDG user bin (luban.exe + luban-shim.exe live here,
                                        # alongside uv / pipx / claude-code / etc.)

%LOCALAPPDATA%\luban\
  toolchains\
    cmake-4.3.2-x86_64\                 # cmake.exe + share/cmake-4.3
    llvm-mingw-20260421-x86_64\         # clang/clang++/clangd/lld/libc++ + sysroot
    ninja-1.13.2-x86_64\
    mingit-2.54.0-x86_64\
    vcpkg-2026.03.18-x86_64\            # ports tree + vcpkg.exe
  bin\                                  # luban-managed shim dir (on PATH after env --user;
    cmake.cmd / cmake.exe / ...         # cargo's ~/.cargo/bin/ pattern). 280+ alias shims.
%USERPROFILE%\.cache\luban\downloads\   # archive cache
%USERPROFILE%\.cache\luban\vcpkg\       # vcpkg downloads + binary cache (XDG-respecting)
%USERPROFILE%\.local\state\luban\       # installed.json (component registry)
%USERPROFILE%\.config\luban\            # selection.json + emscripten config
```

See [XDG-first directory layout](./reference/paths.md) for the full spec.

## Uninstall

One command:

```bat
luban self uninstall --yes
```

That removes HKCU PATH/env entries, wipes
`%LOCALAPPDATA%\luban\` / `%USERPROFILE%\.cache\luban\` /
`%USERPROFILE%\.local\state\luban\` / `%USERPROFILE%\.config\luban\`,
and schedules `luban.exe` + `luban-shim.exe` for self-delete (~1.5s).

Add `--keep-data` to preserve toolchains on disk and only undo the HKCU
env injection.

Luban never writes outside the directories listed above. There is no
Windows Registry persistence beyond HKCU PATH / VCPKG_ROOT / EM_CONFIG
(cleaned up by `luban env --unset-user` or `luban self uninstall`).
