# XDG-first directory layout

Luban respects the [XDG Base Directory Specification](https://specifications.freedesktop.org/basedir-spec/basedir-spec-latest.html), even on Windows. This makes container/CI setups trivial and lets a future Linux port "just work."

## The four canonical homes

Every Luban file lives under one of four roles:

| Role | Purpose |
|---|---|
| `data` | Long-lived state. Toolchain installs, the shim dir, content store, registry mirrors. |
| `cache` | Recreatable artifacts. Downloaded archives, vcpkg binary cache. Safe to delete. |
| `state` | Mutable runtime state. `installed.json`, `.last_sync`, logs. |
| `config` | User preferences. `selection.json`, future `config.toml`. |

## Resolution order (per role)

For each role, in this exact order, the **first** existing answer wins:

1. **`$LUBAN_PREFIX/<role>`** if `LUBAN_PREFIX` is set. Useful for containers / CI / multi-tenant setups.
2. **`$XDG_<ROLE>_HOME/luban`** if the relevant XDG variable is set. (`XDG_DATA_HOME`, `XDG_CACHE_HOME`, `XDG_STATE_HOME`, `XDG_CONFIG_HOME`)
3. **macOS default** (`~/Library/...`) if running on macOS
4. **Windows fallback** (`%LOCALAPPDATA%\luban` etc.) if running on Windows
5. **Linux default** (`~/.local/share/luban` etc.) — the spec's fallback

> **Note**: on Linux the 3rd and 4th rows don't apply, so it's straight to the Linux default. On Windows, `XDG_*` env vars take precedence over `%LOCALAPPDATA%`.

## Default paths per platform

| Role | Linux default | Windows fallback | macOS default |
|---|---|---|---|
| data | `~/.local/share/luban` | `%LOCALAPPDATA%\luban` | `~/Library/Application Support/luban` |
| cache | `~/.cache/luban` | `%LOCALAPPDATA%\luban\Cache` | `~/Library/Caches/luban` |
| state | `~/.local/state/luban` | `%LOCALAPPDATA%\luban\State` | `~/Library/Application Support/luban/State` |
| config | `~/.config/luban` | `%APPDATA%\luban` (roaming) | `~/Library/Preferences/luban` |

## Sub-locations

Within each role, Luban creates a fixed set of sub-directories on first run:

```text
<data>/
  toolchains/<name>-<ver>-<arch>/    # one dir per installed component
  bin/                                # rustup-style shim dir (added to user PATH)
  env/activate.{cmd,ps1,sh}           # generated activate scripts + default.env.json
  registry/buckets/<bucket>/bucket/*.json    # cached Scoop bucket manifests
  registry/overlay/*.json             # user-supplied overlay manifests
  store/sha256/<sha>/blob             # content-addressable file store (M2.5+: hardlink dedup)

<cache>/
  downloads/                          # archive downloads (zip etc.)
  vcpkg-binary/                       # vcpkg binary cache (when configured)

<state>/
  installed.json                      # component registry
  .last_sync                          # bucket sync timestamp
  logs/                               # future: install/build logs

<config>/
  selection.json                      # which components luban setup installs
  config.toml                         # future: user preferences
```

Run `luban doctor` to see the resolved paths on your machine.

## Why XDG even on Windows

Three reasons:

1. **Container/CI portability** — a single `LUBAN_PREFIX=/tmp/luban` env var redirects everything for headless builds, with no Windows API quirks.
2. **No-surprise Linux port** — when Linux support lands, the same paths work; users moving cross-platform don't need to relearn anything.
3. **Cleanly separated concerns** — `data` (long-lived), `cache` (rm-safe), `state` (mutable), `config` (user-edited) are different categories with different backup semantics. Lumping into a single `%LOCALAPPDATA%\luban` would hide that.

## Overrides for power users

```bat
:: redirect everything (good for containers, ephemeral CI)
set LUBAN_PREFIX=C:\luban-isolated

:: redirect just data dir (e.g., put toolchains on a different drive)
set XDG_DATA_HOME=D:\luban-cache\share

:: same for the others
set XDG_CACHE_HOME=...
set XDG_STATE_HOME=...
set XDG_CONFIG_HOME=...
```

After setting any of these, re-run `luban doctor` to confirm Luban sees the override.

## See also

- [`luban doctor`](../commands/doctor.md) — print resolved paths and verify everything exists
- [Architecture → Design philosophy](../architecture/philosophy.md) §4 (XDG rationale)
