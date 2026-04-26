# First-time machine setup

A complete walkthrough from a fresh Windows install to a working C++ project.

## Assumptions

- Windows 10 (1809+) or Windows 11, x64
- A user account with normal (non-admin) rights
- Network access

You do **not** need: Visual Studio, MSYS2, Chocolatey, Scoop, Python, Git, or anything else pre-installed.

## Step 1 — Get `luban.exe`

Any of:

- Download from [GitHub Releases](https://github.com/your-org/luban/releases) and put it in `%USERPROFILE%\bin\luban.exe` (or anywhere)
- `git clone` + build from source if you already have a C++ toolchain (see [Installation](../install.md))

Open a **fresh** Command Prompt or PowerShell. Verify:

```bat
where luban
:: should print full path; if not, you need to use full path or add to PATH temporarily
```

## Step 2 — Install the toolchain

```bat
luban setup
```

Expected output (truncated):

```text
→ Ensuring canonical directories exist
✓ directories ready
→ Deploying seed manifests + selection
✓ overlays on disk: 3
→ Installing 5 component(s)
→ → llvm-mingw
→ download llvm-mingw-20260421-ucrt-x86_64.zip
  [########################] 100.0%  178.4 MiB/178.4 MiB  ...
→ extract llvm-mingw-20260421-ucrt-x86_64.zip
✓ installed llvm-mingw 20260421 → C:\Users\you\.local\share\luban\toolchains\llvm-mingw-20260421-x86_64
→ → cmake
...
→ → ninja
...
→ → mingit
...
→ → vcpkg
→ extract 2026.03.18.zip
→ bootstrapping vcpkg.exe (microsoft/vcpkg-tool)
✓ vcpkg.exe ready
✓ installed vcpkg 2026.03.18 → ...
✓ setup complete
```

**Time**: 3–5 minutes on a typical home connection. Most of it is downloading the LLVM-MinGW archive (~180 MB compressed).

## Step 3 — Register on PATH

```bat
luban env --user
```

Output:

```text
✓ added C:\Users\you\.local\share\luban\bin to HKCU PATH
✓ set HKCU LUBAN_DATA = C:\Users\you\.local\share\luban
✓ set HKCU LUBAN_CACHE = C:\Users\you\.cache\luban
✓ set HKCU LUBAN_STATE = C:\Users\you\.local\state\luban
✓ set HKCU LUBAN_CONFIG = C:\Users\you\.config\luban
✓ set HKCU VCPKG_ROOT = C:\Users\you\.local\share\luban\toolchains\vcpkg-2026.03.18-x86_64
· open a new shell for the changes to take effect.
```

> ⚠️ **Critical**: Close the current cmd/PowerShell window. Open a **fresh one**. The PATH change affects new processes, not the one running luban.

## Step 4 — Verify

In your fresh shell:

```bat
cmake --version
:: cmake version 4.3.2

clang --version
:: clang version 22.1.4 ...

ninja --version
:: 1.13.2

clangd --version
:: clangd version 22.1.4 ...

vcpkg --version
:: vcpkg package management program version 2026-03-04-...
```

If any of these fail with "not recognized as a command":

1. Confirm you opened a **new** terminal (the change doesn't propagate to running shells)
2. `luban doctor` — should show all components ✓ and tools ✓ on PATH
3. `echo %PATH%` — `<data>\luban\bin` should be near the front

## Step 5 — Hello, world

```bat
mkdir %USERPROFILE%\projects
cd %USERPROFILE%\projects

luban new app hello
:: → Scaffolding app 'hello' at C:\Users\you\projects\hello
:: ✓ wrote 12 files
:: → running initial `luban build` to produce compile_commands.json
:: ...
:: ✓ build complete
:: → next: cd hello && nvim src/hello/main.cpp  (clangd ready out of the box)

cd hello
build\default\src\hello\hello.exe
:: hello from hello!
```

You're up. Open `src\hello\main.cpp` in your favorite editor; clangd should attach automatically and give autocomplete.

## Where to next

- [Add a vcpkg library](./add-vcpkg-dep.md) — your second project will probably want one
- [The Daily Driver Loop](../daily-loop.md) — what week 2 looks like
- [IDE integration](./ide-integration.md) — Neovim, VS Code, JetBrains
