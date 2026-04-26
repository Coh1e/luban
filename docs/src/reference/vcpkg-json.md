# `vcpkg.json` & `vcpkg-configuration.json`

Two files vcpkg uses for manifest mode. **luban-managed projects always use manifest mode.**

## `vcpkg.json` — the manifest

This is **vcpkg's** schema, not luban's. luban only edits the `dependencies` array (and `name` / `version` on `luban new`).

```jsonc
{
  "name": "myapp",
  "version": "0.1.0",
  "dependencies": [
    "fmt",                                          // baseline version
    {"name": "spdlog", "version>=": "1.13.0"},      // version constraint
    "nlohmann-json"
  ],
  "overrides": [                                    // optional: pin exact versions
    {"name": "fmt", "version": "10.2.1"}
  ],
  "features": {                                     // optional: opt-in optional feature deps
    "tests": {
      "description": "build with tests",
      "dependencies": ["catch2"]
    }
  }
}
```

`luban add` writes `dependencies`. Other sections — `overrides`, `features`, `supports`, `builtin-baseline`, `dependencies[].features` — are preserved as-is when luban writes the file (extras are copied through unchanged).

See vcpkg's [manifest reference](https://learn.microsoft.com/en-us/vcpkg/reference/vcpkg-json) for full schema.

## `vcpkg-configuration.json` — the baseline pin

Locks the **exact commit of microsoft/vcpkg ports tree** that defines version-to-port-revision mappings.

```json
{
  "default-registry": {
    "kind": "git",
    "repository": "https://github.com/microsoft/vcpkg",
    "baseline": "c3867e714dd3a51c272826eea77267876517ed99"
  }
}
```

`baseline` is a 40-char git commit SHA, **not** a tag. `luban new` writes this with the most-recently-known stable vcpkg commit. Bump it intentionally:

```bat
:: M3 will provide:  luban sync --baseline-now
:: Until then, hand-edit vcpkg-configuration.json:
git -C %VCPKG_ROOT% rev-parse HEAD     # get current ports tree commit
:: paste that into baseline field
luban sync                              # noop semantically, just confirms parse
```

Why a baseline matters: without it, vcpkg uses HEAD which moves under you, breaking reproducibility ([reproduce workflow](../workflows/reproduce.md)).

## What luban does NOT touch

- `vcpkg-configuration.json` after `luban new` — this is yours
- `vcpkg.json[overrides]`, `vcpkg.json[features]` — preserved exactly
- `vcpkg-cache/` or `vcpkg_installed/` — those are vcpkg's working dirs, both `.gitignore`d
