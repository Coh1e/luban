# `luban sync`

Re-read `vcpkg.json` + `luban.toml` in the current project, regenerate `luban.cmake`.

```text
luban sync
```

## When you'd use it

- Just **pulled** a project from git, want to make sure `luban.cmake` is up to date with the in-tree `vcpkg.json`
- Hand-edited `vcpkg.json` (e.g., to add `"features"` or `"overrides"` that `luban add` doesn't expose)
- Changed `luban.toml [scaffold] warnings = "strict"` and want it reflected
- `luban.cmake` got accidentally deleted

## What it changes

Only `luban.cmake`. `vcpkg.json` and `luban.toml` are read-only inputs.
