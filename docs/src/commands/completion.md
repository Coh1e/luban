# `luban completion`

Generate shell completion scripts.

```text
luban completion <shell>
```

Today only **clink** (cmd.exe enhancement) is supported. Future shells: bash, zsh, pwsh.

## `luban completion clink`

Print a clink Lua argmatcher script to stdout. Redirect into your clink completions directory:

```bat
luban completion clink > %LOCALAPPDATA%\clink\completions\luban.lua
```

Restart cmd.exe (or run `clink reload`) and tab completion is live.

### What you get

```text
luban a<TAB>                 → add
luban target a<TAB>          → add
luban target add l<TAB>      → lib
luban self <TAB>             → update / uninstall
luban self uninstall --<TAB> → --yes / --keep-data
luban new <TAB>              → app / lib
luban env --<TAB>            → --apply / --user / --unset-user
```

### Prerequisites

clink must be installed and active in cmd.exe. Quick install via scoop:

```bat
scoop install clink
```

Or download from [chrisant996/clink](https://github.com/chrisant996/clink/releases). Verify with `clink --version` inside cmd.exe.

### How the script is generated

The Lua script is a **static string literal** baked into `luban.exe`. It does not introspect the CLI registry at runtime — we maintain it by hand alongside new verbs. This keeps `luban completion clink` cheap and offline-only (no PowerShell, no fork-exec, just `WriteFile(stdout)`).

The trade-off: if a verb is added without updating `src/commands/completion.cpp`, the new verb still works on the command line but won't tab-complete until the next release. Acceptable: the canonical CLI surface is small (16 verbs) and changes in lockstep with releases.

### Why clink (and not native cmd)?

Native cmd.exe has zero programmable argument completion — only filename completion via `cmd /F:ON`. clink injects a readline-style line editor with full Lua scriptability, used by tens of thousands of cmd users for git/scoop/winget completion.

### Future shells

`luban completion <shell>` rejects unknown shells with a friendly error pointing at the issue tracker. PRs welcome for:

- `bash` — for Git Bash / WSL users running luban.exe
- `pwsh` — PowerShell 7+ argument completers
- `zsh` — for Linux/macOS port (M3 deferred)

Until then: `clink` is the recommended path on Windows.
