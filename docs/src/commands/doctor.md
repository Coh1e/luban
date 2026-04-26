# `luban doctor`

Print luban's view of the world. Use this when something seems off.

## What it shows

```text
→ Canonical homes
  ✓ data    C:\Users\you\.local\share\luban
  ✓ cache   ...
  ✓ state   ...
  ✓ config  ...

→ Sub-directories
  ✓ store               ...
  ✓ toolchains          ...
  ✓ bin                 ...
  ...

→ Installed components
  ✓ cmake                        4.3.2
  ✓ llvm-mingw                   20260421
  ...

→ Tools on PATH
  ✓ clang          C:\Users\you\.local\share\luban\bin\clang.cmd
  ✓ cmake          ...
  · vcpkg          (not found)        ← if vcpkg not yet installed or PATH not set
```

## When to run it

- After `luban setup`, before doing anything else
- When a new tool you expect to be there (e.g., `clang-tidy`) fails to launch
- When migrating to a new machine — confirms your `<data>/<cache>/<state>/<config>` resolution is sane

## Reading the output

- ✓ green check + path = installed and resolvable
- · grey dot + `(not found)` = not installed, **or** installed but not on this shell's PATH
- ✗ red cross = something's actively wrong (e.g., `installed.json` is corrupt)

If a tool shows `(not found)` but `installed.json` lists it: open a fresh shell after `luban env --user`, or `call <data>\env\activate.cmd`.
