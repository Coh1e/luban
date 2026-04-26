# IDE integration

Luban-managed projects work in any editor that speaks LSP, with **zero configuration**. The contract is:

1. `clangd` is on PATH (provided by `luban env --user`)
2. `compile_commands.json` is at the project root (produced by `luban build`)
3. cmake is on PATH (so `cmake --preset` works for any "configure-via-cmake-tools" extensions)

If those three are true, the IDE attaches clangd, finds the right toolchain, and gives you autocomplete + diagnostics + jump-to-definition out of the box.

## Neovim

If you use [`nvim-lspconfig`](https://github.com/neovim/nvim-lspconfig):

```lua
require('lspconfig').clangd.setup{}
```

That's it. clangd will find `compile_commands.json` automatically (project root + a few common subdirs).

## VS Code / Cursor

Install [clangd extension](https://marketplace.visualstudio.com/items?itemName=llvm-vs-code-extensions.vscode-clangd). Done.

The clangd extension auto-detects `compile_commands.json`. The bundled C/C++ extension by Microsoft is **not** needed for clangd workflow (and conflicts with it; disable for this workspace).

For cmake-tools experience (preset selector, build/run buttons), also install [CMake Tools extension](https://marketplace.visualstudio.com/items?itemName=ms-vscode.cmake-tools) — it picks up `CMakePresets.json` automatically.

## CLion / RustRover / other JetBrains

CLion auto-detects cmake projects via `CMakePresets.json`. Just open the project root.

## What if it doesn't work?

Things to check:

1. **`luban env --user` was run** in a previous shell? Open a fresh terminal and re-check `clangd --version`.
2. **`luban build` was run at least once**? `compile_commands.json` only exists after a build. `luban new` runs a build automatically; if you passed `--no-build`, run `luban build` manually.
3. **`compile_commands.json` is in the project root**? Should be a copy of `build/<preset>/compile_commands.json`. If not, `luban build` re-syncs it.
4. **Right clangd version**? `which clangd` should point at `<data>/luban/bin/clangd.cmd` (or, after future M3, `clangd.exe`). Stale system clangd may shadow ours.

## Why it works without config

clangd reads `compile_commands.json` (a list of `{file, command, directory}` JSON objects produced by cmake). Each entry has the full compile command, including `-I`, `-D`, `-std=...`, and the actual `clang++.exe` absolute path. clangd uses that compile command verbatim — no guessing.

Because the path to `clang++.exe` is absolute (luban-managed toolchain), clangd auto-discovers the matching system headers / libc++ via the compiler's resource directory. You never need to set `--query-driver` or `compile_flags.txt`.

## Why "compile_commands.json at project root" matters

clangd searches a fixed list of locations for `compile_commands.json`:
1. `<project>/compile_commands.json`
2. `<project>/build/compile_commands.json`
3. `<project>/build/<some-preset>/compile_commands.json`

`luban build` always copies to (1), the most reliable location. This is plain cmake convention; any IDE with clangd integration knows about it.
