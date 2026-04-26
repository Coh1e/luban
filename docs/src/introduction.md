# Luban

> Windows-first C++ toolchain manager + cmake/vcpkg auxiliary frontend.
> Single static-linked binary, zero UAC, XDG-first directories.
>
> 🌐 **[中文版 →](/zh/)**

## What problem Luban solves

If you've ever tried to set up a modern C++ toolchain on Windows, you've met the wall:

- pick LLVM-MinGW or MSVC or mingw-w64;
- install cmake, ninja, clangd, vcpkg separately;
- discover that vcpkg needs git;
- learn cmake's quirks, vcpkg manifest mode, CMakePresets, triplets;
- write a CMakeLists.txt that conditionally pulls in `find_package` + `target_link_libraries` for each dep;
- realize clangd needs `compile_commands.json` to give you autocomplete;
- realize none of this is on `PATH` until you run an activate script in every fresh shell.

Each piece is a half-day of yak-shaving. The full stack-up usually takes a few days, and rebuilds itself on every new machine.

**Luban replaces the yak-shaving with two commands:**

```text
luban setup          # one-time, installs LLVM-MinGW + cmake + ninja + mingit + vcpkg
luban env --user     # one-time, registers everything on user PATH (rustup-style)
```

After that, fresh shells have `cmake`, `clang++`, `ninja`, `clangd` directly. Then for each project:

```text
luban new app foo    # scaffold; auto-builds; clangd ready out of the box
cd foo
luban add fmt        # one-line dep add
luban build          # cmake fetches the dep via vcpkg manifest mode
```

**You never open `CMakeLists.txt` for the 80% case.**

## What Luban is not

- **Not a build system.** cmake + ninja still do the building. Luban only generates the cmake glue.
- **Not a package manager.** vcpkg still resolves and builds C++ packages. Luban edits `vcpkg.json` for you.
- **Not a replacement.** You can `git clone` a Luban-managed project and build it on a machine without Luban — `cmake --preset default` works as long as cmake + vcpkg are present. Luban-generated files (`luban.cmake`) commit cleanly to the repo.

## Design philosophy

| Principle | What it means |
|---|---|
| **Auxiliary, not authoritative** | cmake / vcpkg / ninja stay primary. Luban writes `luban.cmake` (a standard cmake module) that the user `include()`s. Drop one line and luban is gone. |
| **No new manifest format** | Deps live in `vcpkg.json` (vcpkg's own schema). `luban.toml` is *optional* and only holds project-level *preferences* (warning level, sanitizers). |
| **`luban.cmake` is git-tracked** | Reproducibility: cloning the project on a Luban-free machine still builds. |
| **XDG-first directories** | `~/.local/share/luban/` (data), `~/.cache/luban/` (cache), `~/.local/state/luban/` (state), `~/.config/luban/` (config). `XDG_*` env vars override. |
| **Zero UAC** | All Luban toolchains land in user-writable directories. No admin prompts ever. |
| **Single static binary** | One `luban.exe` with everything. No Python, no MSI, no installer. |

## Where to next

- [Installation](./install.md) — getting `luban.exe` on your machine
- [Quickstart](./quickstart.md) — the 5-command sequence to "hello, fmt"
- [The Daily Driver Loop](./daily-loop.md) — what a typical week with luban looks like
- [Commands → Overview](./commands/overview.md) — full command reference
