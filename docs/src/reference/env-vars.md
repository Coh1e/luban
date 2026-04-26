# Environment variables

Variables luban reads or writes.

## Variables luban reads

| Variable | Effect |
|---|---|
| `LUBAN_PREFIX` | If set, all four roles resolve to `$LUBAN_PREFIX/<role>`. Highest priority. |
| `XDG_DATA_HOME` | Override `<data>` to `$XDG_DATA_HOME/luban`. |
| `XDG_CACHE_HOME` | Override `<cache>` to `$XDG_CACHE_HOME/luban`. |
| `XDG_STATE_HOME` | Override `<state>` to `$XDG_STATE_HOME/luban`. |
| `XDG_CONFIG_HOME` | Override `<config>` to `$XDG_CONFIG_HOME/luban`. |
| `LOCALAPPDATA` | Windows fallback for data/cache/state. |
| `APPDATA` | Windows fallback for config (roaming). |
| `USERPROFILE` / `HOMEDRIVE`+`HOMEPATH` / `HOME` | Used to find the user's home dir. |
| `NO_COLOR` | If set (any value), disables ANSI colors in luban's output. |
| `LUBAN_NO_PROGRESS` | If set, disables download progress bar (CI-friendly). |
| `PATH` | Read by `luban env --user` to deduplicate before adding `<data>/bin`. |

## Variables luban *writes* (with `luban env --user`)

These are written to `HKCU\Environment` (current user, persistent).

| Variable | Value |
|---|---|
| `PATH` | Prepends `<data>/luban/bin` (deduped, idempotent) |
| `LUBAN_DATA` | `<data>` resolved at the time of `luban env --user` |
| `LUBAN_CACHE` | `<cache>` |
| `LUBAN_STATE` | `<state>` |
| `LUBAN_CONFIG` | `<config>` |
| `VCPKG_ROOT` | Path to currently-installed vcpkg toolchain dir (empty if vcpkg not installed yet) |

`luban env --unset-user` removes all of the above.

## Variables consumed by tools, not luban itself

- `VCPKG_ROOT` is read by vcpkg's cmake integration, NOT by luban directly. We just set it.
- `CMAKE_TOOLCHAIN_FILE` referenced in `CMakePresets.json` uses `$env{VCPKG_ROOT}` to find vcpkg's toolchain file at cmake configure time.

## Generated activate scripts

`<data>/luban/env/activate.{cmd,ps1,sh}` set the same variables (PATH + LUBAN_*) for the **current shell session**. Useful when:

- You can't or don't want to use `luban env --user` (CI, ephemeral containers)
- You need a per-shell setup (e.g., test multiple luban prefixes via `LUBAN_PREFIX`)

Source as appropriate:

```bat
:: cmd
call %LOCALAPPDATA%\luban\env\activate.cmd

:: pwsh
. $env:LOCALAPPDATA\luban\env\activate.ps1

:: bash / WSL
source ~/.local/share/luban/env/activate.sh
```
