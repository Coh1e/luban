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

- As of v0.2, real `.exe` proxies (hard-linked to `luban-shim.exe`, rustup-style) are written alongside each `.cmd` shim. cmake's compiler-detect now sees them as "real" cmake/clang.
- The `.cmd` shims remain for interactive shell use (faster start than going via the proxy when launched from cmd directly).
