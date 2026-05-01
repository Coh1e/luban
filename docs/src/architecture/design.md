# Design summary

A single page covering the question: **why does luban look the way it does?**

If you read only one architecture page, read this. The other architecture
pages (`philosophy`, `two-tier-deps`, `why-cmake-module`, `roadmap`) zoom
into individual topics — this is the synthesis.

## What problem luban solves

Modern C++ on Windows requires the user to assemble:

1. A toolchain (LLVM-MinGW or MSVC + MinGW or mingw-w64)
2. cmake — project meta-build
3. ninja — actual builder
4. clangd — LSP for editors
5. vcpkg — package manager
6. git — for vcpkg + general use
7. Some glue: `compile_commands.json`, `find_package`, `target_link_libraries`,
   triplets, `CMakePresets`, `CMAKE_TOOLCHAIN_FILE`, `vcpkg-configuration.json`
   baseline pinning, `target_include_directories(... PUBLIC)`, etc.

Each of these is half a day of yak-shaving. Stack-up usually takes days,
and resets on every new machine.

luban's job: turn that into **two one-time commands** plus
**a tiny per-project loop**:

```bat
luban setup                       :: 〜3 min, ~250 MB toolchains
luban env --user                  :: register on HKCU PATH (rustup-style)
:: open fresh shell

luban new app foo
cd foo
luban add fmt                     :: edits vcpkg.json + luban.cmake
luban build                       :: vcpkg auto-fetches fmt during configure
```

User never opens `CMakeLists.txt`, never writes a `find_package`, never
reads vcpkg manifest mode docs.

## What luban is NOT

- **Not a build system.** cmake + ninja still do the building.
- **Not a package manager.** vcpkg still resolves and builds C++ packages.
- **Not a fork or replacement.** A luban-managed project remains a *standard
  cmake project* — clone it onto a luban-free machine, `cmake --preset
  default && cmake --build --preset default` works.

luban is **glue with opinions**: the right glue, glued correctly, with
opinionated defaults that keep beginners on the rails.

## The 8 architecture invariants

Locked-in design decisions. Breaking any of them invalidates luban's
fundamental contract:

### 1. cmake stays primary

luban does not invent a new IR. luban does not invent a new manifest format
to replace `CMakeLists.txt`. luban writes a *standard cmake module*
(`luban.cmake`) that user code `include()`s. One line removed and luban is gone.

### 2. `luban.cmake` is git-tracked

The whole point of "auxiliary" is that the project must remain buildable
without luban. `luban.cmake` is a regular cmake module, committed to git,
always reproducible.

### 3. No new manifest format

Project deps live in `vcpkg.json` (vcpkg's schema). luban edits it via
`luban add`. There is no parallel `[deps]` section in `luban.toml` —
that would create double truth.

`luban.toml` exists only as **optional project preferences** (warning
level, sanitizers, default preset, triplet) — the kinds of things that
don't fit naturally in vcpkg.json.

### 4. `luban.toml` is optional

Many luban projects have no `luban.toml` at all. It only appears when the
user has actual preferences. Defaults work for the 80% case.

### 5. XDG-first paths, even on Windows

luban respects `XDG_DATA_HOME` / `XDG_CACHE_HOME` / `XDG_STATE_HOME` /
`XDG_CONFIG_HOME` and the `LUBAN_PREFIX` umbrella variable, *before*
falling back to `%LOCALAPPDATA%` / `%APPDATA%` defaults.

This makes container/CI/multi-user setups trivial, and makes a future
Linux port a no-surprise affair.

### 6. Zero UAC

Every luban-managed file lives in user-writable directories. luban never
writes `Program Files`, never touches `HKLM`, never spawns elevated
installers. `luban env --user` writes only to `HKCU` (current user).

### 7. Single static-linked binary

`luban.exe` is one file: ~3 MB release / ~32 MB debug, depending on
miniz/json/toml++ vendored single-headers. No DLLs alongside, no Python,
no MSI. Drop on a USB stick, copy to a fresh VM, runs.

### 8. Two-tier dependency model

| Layer | What | Where | Who |
|---|---|---|---|
| **System** | cmake, ninja, clang, clangd, lld, git, vcpkg | `<data>/toolchains/` | `luban setup` |
| **Project** | fmt, spdlog, boost, catch2 | `<project>/vcpkg_installed/<triplet>/` | `luban add` (which edits vcpkg.json) |

Mixing them is rejected. `luban add cmake` errors out with a hint about
`luban setup`. System tools are version-locked machine-wide; project libs
live per-project with their own baseline pin.

## Why this shape (the alternatives we rejected)

### Alternative A: a luban DSL → lower to cmake

xmake / meson / build2 / premake all chose this path. User writes
`xmake.lua` or `meson.build`, the tool generates the actual build files.

**Why we said no**: C++ has decades of cmake idioms. Custom commands,
target-specific flags, conditional features — every real project hits cases
the DSL doesn't cover and falls through to "you have to know cmake anyway."
At that point the DSL is just an extra thing to learn that doesn't hide
cmake. Worse, the seam between DSL and cmake is hostile (eject rituals,
one-way conversions).

### Alternative B: in-place edit of `CMakeLists.txt` with marker blocks

Like a code generator inserting `# >>> BEGIN luban` / `# <<< END luban`
sections. Luban owns those blocks; user owns the rest.

**Why we said no**: edit-in-place is fragile. User's hand-edits inside the
markers get clobbered. Edits *near* the markers race with regeneration.
The boundary is a constant source of confusion.

`luban.cmake` solves this cleanly: luban owns the *file*, user `include()`s
it, the boundary is a hard line.

### Alternative C: One unified manifest at the top

`luban.toml` containing everything — deps, scripts, profiles, workspace,
toolchain pin. Cargo style.

**Why we said no**: vcpkg's `vcpkg.json` already exists and is the de facto
C++ manifest. Inventing a parallel `[deps]` section means we maintain a
double-truth, with constant drift between the two. Better to let vcpkg
own that schema and just edit it via `luban add`.

### Alternative D: A luban-init.exe bootstrapper

rustup-style: a tiny installer that downloads luban + runs setup.

**Why we said no**: luban.exe is *already* a single self-contained binary.
Users `curl -O luban.exe` from GitHub Releases, run it. The bootstrapper
adds a layer with no real value. Instead, `luban self update` and
`luban self uninstall` round-trip the lifecycle within one binary,
uv-style.

## Trade-offs we accept

- **Windows-first.** Linux/macOS port is M3+ work; we lose multi-platform
  hygiene short-term but gain rapid iteration on the platform that needs
  the most help (Windows C++ tooling).
- **Vendored deps only.** No `find_package(zlib)`-style consumption of
  system libs. Adds ~10MB to the binary but keeps "one file, runs anywhere"
  promise.
- **The `.cmake`-module model** assumes users will `include(luban.cmake)`.
  If they don't, none of the auto-find_package magic happens — they fall
  through to plain cmake. This is by design.
- **Curated pkg→target mapping.** `luban add` knows ~224 popular libs by
  name. Unknown ports get `find_package(<port>)` written but no auto-link;
  user fills target name in. Long tail handled by future scraping of
  vcpkg's `usage` files.

## Operating model

luban operates as **three layers**, each with stable interfaces:

```
┌─ user ────────────────────────────────────────────────┐
│   luban CLI (16 verbs in 4 groups)                    │
└──────────────────────────────────────────────────────┘
            │ edits vcpkg.json / luban.toml
            │ runs cmake / vcpkg
            ▼
┌─ luban-managed artifacts ─────────────────────────────┐
│   luban.cmake     ← luban writes; git-tracked         │
│   <data>/bin/     ← shim dir; on user PATH            │
│   <state>/installed.json   ← component registry       │
└──────────────────────────────────────────────────────┘
            │ shells out to
            ▼
┌─ external tools ──────────────────────────────────────┐
│   cmake / ninja / clang / vcpkg / git                 │
│   (luban downloads + manages them, but doesn't        │
│    invade their config — they remain "off the shelf") │
└──────────────────────────────────────────────────────┘
```

The middle layer is what makes luban "auxiliary": it's a thin, stable,
git-trackable spec that bridges user intent to standard tools. Drop the
middle layer and you still have working tools at the bottom.

## Where to read more

- [Philosophy](./philosophy.md) — the "auxiliary, not authoritative" stance
  expanded
- [Two-tier dependency model](./two-tier-deps.md) — system vs project deps
  in detail
- [Why no IR, why luban.cmake](./why-cmake-module.md) — alternative B
  (marker-block in-place edit) rejection rationale
- [Roadmap](./roadmap.md) — what's done, what's next
- [Quickstart](../quickstart.md) — the 5-command tour
