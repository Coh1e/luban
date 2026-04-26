# `luban.toml` schema

Project-local preferences for luban. **Optional** — the file does not need to exist; defaults apply.

## Schema (v1)

```toml
[project]
default_preset = "default"          # luban build picks this if --preset not given
triplet        = "x64-mingw-static" # vcpkg target triplet
cpp            = 23                 # C++ standard (passed to luban_apply via cxx_std_23)

[scaffold]
warnings   = "strict"               # off | normal | strict
sanitizers = ["address", "ub"]      # passed to -fsanitize=...
```

| Field | Type | Default | Effect |
|---|---|---|---|
| `[project] default_preset` | string | `"default"` | which preset `luban build` picks if `--preset` not given (and not auto) |
| `[project] triplet` | string | `"x64-mingw-static"` | vcpkg target triplet for manifest-mode installs |
| `[project] cpp` | int | `23` | C++ language standard, passed to cmake `cxx_std_<N>` |
| `[scaffold] warnings` | string | `"normal"` | `off` / `normal` / `strict` (more flags at higher levels) |
| `[scaffold] sanitizers` | array<string> | `[]` | comma-joined and prefixed with `-fsanitize=` for both compile and link |

## What `warnings` controls

Generated in `luban.cmake` via `target_compile_options(... PRIVATE ...)`:

| Level | Flags emitted |
|---|---|
| `off` | (none) |
| `normal` | `-Wall -Wextra` |
| `strict` | `-Wall -Wextra -Wpedantic -Werror=return-type` |

MSVC is detected and these flags are skipped (MSVC doesn't grok GCC-style `-W` flags).

## Regenerating after editing

```bat
luban sync       # rewrites luban.cmake from current vcpkg.json + luban.toml
```

`luban add` and `luban remove` also implicitly regenerate.

## Future fields (M3+)

- `[toolchain]` — pin per-project toolchain versions (rust-toolchain.toml-equivalent)
- `[scripts]` — npm-style command aliases (`luban run <script>`)
- `[workspace]` — multi-package projects (cargo workspace-equivalent)

These are NOT in v1 — adding them would expand the file's role beyond "preferences" toward "manifest", which the design explicitly rejects.
