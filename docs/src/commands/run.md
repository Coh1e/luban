# `luban run`

uv-style transparent activate-and-exec. Run any luban-managed exe with the
toolchain env injected, no `call activate.cmd` needed.

## Synopsis

```text
luban run <cmd> [args...]
```

All args after `<cmd>` are forwarded to the child process **verbatim** —
luban does not parse them. So `luban run cmake --version` sends `--version`
to cmake (not luban).

## How `<cmd>` resolves

In order:

1. **Registry alias** — looked up in `<state>/installed.json`. `cmake` →
   `<data>/toolchains/cmake-X/bin/cmake.exe`. This bypasses the `.cmd` shim
   layer (one less cmd.exe wrapper).
2. **Augmented PATH search** — if no registry match, search luban's
   toolchain bin dirs prepended to your current PATH, with PATHEXT
   (`.exe → .cmd → .bat → no-ext`). Useful for tools not in the registry
   (e.g., `where`, `tasklist`).

If neither finds it, exits 127 with a helpful hint.

## Environment injection

While the child runs, these env vars are set:

- `PATH` = `<luban toolchain dirs>` + your existing PATH
- `LUBAN_DATA` / `LUBAN_CACHE` / `LUBAN_STATE` / `LUBAN_CONFIG`
- (`VCPKG_ROOT` is **not** auto-injected by `run` — set it via `luban env
  --user` once for permanence)

## Examples

```bat
luban run cmake --version
:: cmake version 4.3.2
:: ...

luban run clang -E -dM -x c nul
:: dump preprocessor macros that clang sees by default

luban run vcpkg search fmt
:: equivalent to `luban search fmt`

luban run clang-format -i src\foo.cpp
:: format a single file via Luban's clang-format
```

## When you don't need `luban run`

- `luban env --user` already on this machine → cmake / clang / clangd are on
  user PATH; no need for `luban run` for those.
- Project-local `luban build` → directly wraps cmake without needing `luban run`.

`luban run` is for one-off "run X with luban env" without committing to PATH
registration, or for tools you'd rather not put on PATH.

## Vs alternatives

| Tool | Equivalent |
|---|---|
| uv | `uv run <cmd>` |
| cargo | (no direct equivalent — cargo run is project-build) |
| nix | `nix-shell -p X --run "X args"` |
| direnv | (sourcing on cwd entry; passive vs explicit) |
