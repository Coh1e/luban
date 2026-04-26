# `luban env`

Show or modify environment integration. With no flags, prints status.

## Synopsis

```text
luban env [--apply] [--user | --unset-user]
```

## What each flag does

| Flag | Effect |
|---|---|
| (none) | Print env state: `<data>` location, activate-script paths, what's on PATH |
| `--apply` | Rewrite `<data>/env/activate.{cmd,ps1,sh}` from the current registry |
| `--user` | **Register on HKCU PATH** (rustup-style) + set `LUBAN_*` and `VCPKG_ROOT` user env vars |
| `--unset-user` | Reverse `--user` — remove `<data>/bin/` from PATH and unset the env vars |

Run `luban env --help` for examples.

## When to use each mode

- **One-time after `luban setup`**: `luban env --user` so all new shells see cmake/clang/clangd/vcpkg directly.
- **Already use activate scripts**: don't run `--user`; sourcing `activate.{cmd,ps1,sh}` from `<data>/env/` works equivalently per-shell.
- **CI / container**: skip `--user` entirely; set `PATH` and `VCPKG_ROOT` from the env scripts or directly.

## Behavior details

`--user` is **idempotent**: re-running it doesn't duplicate PATH entries. The check is case-insensitive normalized path.

The PATH change is broadcast via `WM_SETTINGCHANGE`, so File Explorer and newly-spawned processes see it immediately. **Already-running shells do NOT see the change** — that's a Windows constraint.
