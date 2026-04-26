# `luban which`

Print the absolute exe path for a luban-managed alias.

```text
luban which <alias>
```

Resolves the alias via `<state>/installed.json` and prints to stdout. Useful
for debugging "which version of cmake am I running?" or for scripts.

## Examples

```bat
luban which cmake
:: C:\Users\you\.local\share\luban\toolchains\cmake-4.3.2-x86_64\bin\cmake.exe

luban which clangd
:: C:\Users\you\.local\share\luban\toolchains\llvm-mingw-20260421-x86_64\bin\clangd.exe

luban which vcpkg
:: C:\Users\you\.local\share\luban\toolchains\vcpkg-2026.03.18-x86_64\vcpkg.exe
```

stdout has just the path (good for piping). Diagnostic info (which component
provides this alias) goes to stderr.

## Multi-component aliases

If multiple components provide the same alias (rare; e.g., if you have both
mingit and a separate git overlay), the **first** match in installed.json
wins, with a stderr warning listing all candidates.

## Vs other tools

- `where cmake` (Windows cmd) — searches PATHEXT in current PATH; finds the
  shim (`cmake.cmd` or `cmake.exe`), not the underlying real binary
- `which cmake` (Git Bash) — same as above, just Unix-style
- `luban which cmake` — finds the **real** target exe via the registry,
  bypassing all shim layers

Use the right one for your need.
