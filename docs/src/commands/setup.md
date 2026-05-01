# `luban setup`

Install all enabled toolchain components. **Run this once per machine.**

## Synopsis

```text
luban setup [--only <name>[,<name>...]] [--with <name>[,<name>...]]
            [--without <name>[,<name>...]] [--force] [--dry-run]
```

## Default behavior

Reads `<config>/luban/selection.json` (or seeds it from `manifests_seed/selection.json` on first run), installs every component with `enabled: true`. Idempotent — re-running is fast unless you pass `--force`.

The default selection installs:

| Component | Source | Approx. size |
|---|---|---|
| llvm-mingw | mstorsjo/llvm-mingw release | ~180 MB |
| cmake | Kitware/CMake release | ~50 MB |
| ninja | ninja-build/ninja release | ~300 KB |
| mingit | git-for-windows/git release | ~40 MB |
| vcpkg | microsoft/vcpkg release + bootstrap-vcpkg.bat | ~10 MB + (vcpkg.exe ~7 MB after bootstrap) |

Total: ~280 MB on disk after extraction (sum of ~50 MB compressed downloads in `<cache>/luban/downloads`).

## Flags

### `--only <name>[,<name>...]`

Install only the named component(s). Comma-separated list. **Overrides** the `enabled` flag in selection.json — useful for installing components disabled by default (like `vcpkg` which has its own overlay). **This run only — does not persist.**

```bat
luban setup --only vcpkg
luban setup --only cmake,ninja
```

### `--with <name>[,<name>...]`

Enable + install named component(s) AND **persist** into `<config>/luban/selection.json` so future `luban setup` (with no flags) re-applies. Recursively pulls in `depends:` from each manifest (DFS post-order — deps installed before dependents).

```bat
luban setup --with emscripten   :: also pulls in node via depends
luban setup --with node,zig     :: opt-in to multiple
luban setup --with doxygen      :: needed by `luban doc`
```

Special components (don't fit the standard "download + extract" pipeline):

- **`msvc`** — luban does NOT install Visual Studio Build Tools (Microsoft's
  installer + EULA + ~5 GB). Instead, `luban env --msvc-init` discovers an
  existing VS install via vswhere and captures vcvarsall env into
  `<state>/msvc-env.json`. After that, `luban build` / `luban run` see the
  MSVC env automatically. See [`luban env`](./env.md) for details.

### `--without <name>[,<name>...]`

Symmetric to `--with`: disables in selection.json AND removes from disk (toolchain dir + shims). Idempotent — silent if the component is not currently installed.

```bat
luban setup --without emscripten      :: keeps node (unless also --without node)
luban setup --without emscripten,node :: full WASM cleanup
```

### `--force`

Reinstall the component even if already present at the same version. Re-downloads if cache is stale, re-extracts, rewrites shims. Good for recovering after a corrupted toolchain dir.

### `--dry-run`

Show what would be installed without doing it. Walks the selection, verifies manifests can be fetched + parsed, prints the URL each component would download. **No network downloads, no extracts, no registry writes.**

## Per-component pipeline

For each enabled component, luban runs:

1. **Resolve manifest** — `<data>/registry/overlay/<name>.json` first (populated
   on first run from `manifests_seed/`), then in-tree `manifests_seed/<name>.json`
   as fallback. **No network**; v0.2 dropped the bucket_sync that fetched
   from `raw.githubusercontent.com` (see ADR-0001 / OQ-7 / `manifest_source.cpp`).
2. **Validate manifest** — reject if `installer`, `pre_install`, `post_install`,
   `uninstaller`, `persist`, or `psmodule` fields are present (these would
   require running PowerShell). Also rejects `.msi` / `.nsis` URLs.
3. **Download** the archive to `<cache>/luban/downloads/`, with retries + sha256
   verification streamed during transfer. Falls back through `luban_mirrors`
   array on network failure (NOT on hash mismatch — that means the manifest's
   hash is wrong, no point trying mirrors).
4. **Extract** to a staging dir under `<data>/toolchains/.tmp-<name>-<ver>/`
5. **Apply `extract_dir`** — descend into the wrapper directory if the archive
   uses one
6. **Promote** staging → `<data>/toolchains/<name>-<ver>-<arch>/` (atomic
   rename, copy fallback for cross-volume)
7. **Special bootstrap** when needed:
   - **vcpkg**: run `bootstrap-vcpkg.bat` once after extract to fetch matching
     `vcpkg.exe` from microsoft/vcpkg-tool releases
   - **emscripten**: write `<config>/emscripten/config` (XDG-respecting since
     v0.2) referencing LLVM_ROOT / NODE_JS / etc. — emcc reads it via
     `EM_CONFIG`, which `luban env --user` writes to HKCU.
8. **Write shims** — `.cmd` text shim + `.exe` hardlink (to `luban-shim.exe`),
   one pair per `bin` alias, into `<data>/bin/`. Pre-v0.2 also wrote `.ps1`
   and extensionless sh; dropped to keep the bin dir tidy.
9. **Update registry** — `<state>/luban/installed.json`

## What it does NOT touch

- `HKCU\Environment` (use `luban env --user` for that, separately)
- `HKLM` (anything system-wide; never)
- The `~/scoop/` directory if you have Scoop installed (we read Scoop manifests, never write the Scoop layout)
- Existing user PATH (until you `luban env --user`)

## Common workflows

### Fresh machine

```bat
luban setup            # ~3 min on a fast connection
luban env --user       # one-time HKCU PATH registration
```

### Add vcpkg later

If the default selection doesn't include vcpkg (depends on which seed you ship):

```bat
luban setup --only vcpkg
luban env --user       # picks up VCPKG_ROOT for new shells
```

### Reinstall a corrupted toolchain

```bat
luban setup --only ninja --force
```

### Verify what's about to happen

```bat
luban setup --dry-run
```

## Failure modes

- **Network**: `luban setup` retries 3× with exponential backoff. Persistent failure → exits 1 with the error from the last attempt.
- **Hash mismatch**: download is discarded, no install. Suggests upstream tampering or a corrupted CDN edge.
- **Unsafe manifest**: aborts with a clear message; user must provide an overlay manifest in `<data>/registry/overlay/<name>.json`.
- **Extract failure**: staging dir is wiped, the partial extract is gone, registry untouched. Safe to retry.

The pipeline is designed so a `Ctrl-C` or system crash never leaves a half-installed component visible to luban — either the install completed and `installed.json` records it, or it didn't.

## See also

- [`luban env`](./env.md) — the rustup-style PATH registration
- [`luban doctor`](./doctor.md) — verify what was installed
- [Reference → Manifest overlay format](../reference/manifest-overlay.md) — how to add components luban doesn't ship by default
