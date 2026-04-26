# `luban self`

Manage the luban binary itself.

```text
luban self update
luban self uninstall [--yes] [--keep-data]
```

## `luban self update`

Pull the latest release from `github.com/Coh1e/luban`, verify SHA256, atomically swap the running `luban.exe`.

### What it does

1. **GET** `https://api.github.com/repos/Coh1e/luban/releases/latest`
2. Compare `tag_name` (e.g. `v0.1.2`) against the running version
3. If equal → exit "already up to date"
4. If newer:
   - Download `luban.exe` to `luban.exe.new` (with retry + stream-SHA)
   - Download `SHA256SUMS`, parse, verify the new exe matches
   - Move running `luban.exe` → `luban.exe.old`
   - Move `luban.exe.new` → `luban.exe`
   - Schedule `luban.exe.old` for delete-on-reboot via `MoveFileExW(... MOVEFILE_DELAY_UNTIL_REBOOT)`
5. Exit; next invocation runs the new binary

The previous binary is **kept** as `luban.exe.old` (auto-cleaned at reboot). If the swap fails partway, the original is restored.

### Behavior on errors

- 4xx HTTP → fail immediately, no retry (URL-issue)
- 5xx / network → 3 retries with exponential backoff
- SHA256 mismatch → refuse swap, delete `.new` file, exit 1
- Move failures → restore original; exit 1

## `luban self uninstall --yes`

Reverse every footprint of luban on this machine.

> ⚠️ **Destructive.** Refuses without `--yes`. Print a plan, then exit 1.

### What it removes

| Step | Effect |
|---|---|
| 1 | `HKCU\Environment` PATH entry pointing at `<data>/bin/` |
| 2 | `HKCU\Environment\LUBAN_DATA`/`LUBAN_CACHE`/`LUBAN_STATE`/`LUBAN_CONFIG` |
| 3 | `HKCU\Environment\VCPKG_ROOT` |
| 4 | `<data>` (~250 MB toolchains + content store) |
| 5 | `<cache>` (download archives + vcpkg binary cache) |
| 6 | `<state>` (`installed.json` + logs) |
| 7 | `<config>` (`selection.json`) |
| 8 | `luban.exe` + `luban-shim.exe` (via deferred batch script) |

### Self-delete on Windows

Running `.exe` files are file-locked by Windows. We can't `del` them while running. Pattern used (rustup-style):

1. Write a temp `luban-uninstall.bat`:
   ```bat
   @echo off
   ping 127.0.0.1 -n 2 >nul     :: ~1.5s delay
   del /F /Q "<luban.exe>"      :: by then we've exited; lock released
   del /F /Q "<luban-shim.exe>"
   del /F /Q "%~f0"             :: batch self-deletes
   ```
2. Spawn the batch via `CreateProcessW` with `DETACHED_PROCESS`
3. Exit luban.exe immediately
4. After ~1.5s, batch wakes up + deletes everything

You'll briefly see no `luban.exe` on disk; this is intentional.

### Flags

| Flag | Effect |
|---|---|
| `--yes` | Required to actually run; without it, prints plan + exits 1 |
| `--keep-data` | Preserve `<data>` etc. (toolchains stay on disk); only undo HKCU env injection + delete the binaries |

### Use cases

- **Full uninstall**: `luban self uninstall --yes` — leaves no trace
- **"Reset" without re-downloading toolchains**: `luban self uninstall --yes --keep-data`, then re-download luban.exe + run `luban setup` (idempotent — re-uses existing `<data>/toolchains/`)
- **Migrate to a different luban version**: prefer `luban self update` over uninstall + reinstall

## Why no separate `luban-init.exe`

rustup ships `rustup-init.exe` because rustup itself is a shell-script-bootstrapped Rust toolchain — there's no chicken-and-egg "first download a single binary that runs". luban already IS that single binary: download `luban.exe` from [Releases](https://github.com/Coh1e/luban/releases), put it where you want, run `luban setup`. `luban self update`/`uninstall` round-trip the entire lifecycle within one binary, uv-style.
