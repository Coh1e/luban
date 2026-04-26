# `luban shim`

Repair tool. Walks `<state>/luban/installed.json` and rewrites every shim file under `<data>/luban/bin/`.

```text
luban shim
```

## When you'd run it

- `<data>/bin/` got accidentally deleted
- A toolchain dir was moved or re-bootstrapped (e.g., vcpkg.exe re-downloaded)
- A new luban version changed shim format and you want to upgrade existing installs

## What gets written

For each `bins` entry in each component record:

```
<data>/bin/<alias>.cmd     # @echo off + "<exe>" %*
<data>/bin/<alias>.ps1     # PowerShell forward
<data>/bin/<alias>         # POSIX sh-script (works in Git Bash, future Linux)
```

Plus mode-bits: the sh-script is `chmod +x` on POSIX, harmless on Windows.

## Notes

- M3 will replace `.cmd` shims with real `.exe` proxies (rustup-style). That'll let cmake's compiler probe accept the shim as a "real" cmake/clang.
- Until then, cmake's compiler-detect uses absolute paths (we point CMakePresets at the toolchain bin directly), and the `.cmd` shims are for interactive shell use.
