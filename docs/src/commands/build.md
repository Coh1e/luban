# `luban build`

Wraps `cmake --preset && cmake --build` and copies `compile_commands.json` to the project root.

## Synopsis

```text
luban build [--preset <name>] [--at <dir>]
```

## Preset auto-selection

If `--preset` is not given (or `--preset auto`), luban picks based on the project's `vcpkg.json`:

| Project state | Picked preset |
|---|---|
| `vcpkg.json` has dependencies | `default` (uses `VCPKG_ROOT` for vcpkg toolchain) |
| `vcpkg.json` is empty / missing | `no-vcpkg` (no toolchain file, faster, no `VCPKG_ROOT` needed) |

Override with `--preset release` etc.

## What it does, in order

1. **Configure**: run `cmake --preset <P>` in the project dir. vcpkg manifest mode kicks in if the toolchain file is set.
2. **Build**: run `cmake --build --preset <P>`. Ninja parallelizes across cores.
3. **Sync**: copy `build/<P>/compile_commands.json` to project root, so clangd / VS Code C/C++ extension auto-find it.
4. **Report**: print `✓ build complete` (or the cmake error verbatim).

The cmake invocation gets `<data>/toolchains/*/bin` prepended to PATH (so cmake/ninja/clang are visible) plus `LUBAN_*` env vars set. **The current process's PATH is preserved underneath**, so vcpkg's child processes can still find `mingit` / system tools.

## Common scenarios

| Goal | Command |
|---|---|
| Just build (auto preset) | `luban build` |
| Force release | `luban build --preset release` |
| Build a subproject | `luban build --at ../other-project` |
| Skip cmake configure (incremental rebuild) | Just run `cmake --build --preset default` directly |
