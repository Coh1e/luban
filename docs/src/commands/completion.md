# `luban completion`

Generate shell completion scripts.

```text
luban completion <shell>
```

Supported shells: **clink** (cmd.exe), **bash**, **zsh**, **fish**, **powershell**.
Each emits a self-contained completion script to stdout — pipe it into the right
location for your shell.

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

### Why clink (and not native cmd)?

Native cmd.exe has zero programmable argument completion — only filename completion via `cmd /F:ON`. clink injects a readline-style line editor with full Lua scriptability, used by tens of thousands of cmd users for git/scoop/winget completion.

## `luban completion bash`

```bash
# system-wide
sudo luban completion bash > /etc/bash_completion.d/luban
# or per-user
luban completion bash > ~/.bash_completion.d/luban
```

Source from `.bashrc` if your distro doesn't auto-load `~/.bash_completion.d/`:

```bash
[ -f ~/.bash_completion.d/luban ] && . ~/.bash_completion.d/luban
```

Git Bash on Windows: drop the file under `~/.bash_completion.d/` and source from `.bashrc`.

## `luban completion zsh`

```zsh
# pick a directory in $fpath (e.g. ~/.zsh/completions)
luban completion zsh > ~/.zsh/completions/_luban
# add to ~/.zshrc once
fpath=(~/.zsh/completions $fpath)
autoload -U compinit && compinit
```

The completion file must be named `_luban` (the underscore prefix is mandatory for zsh's autoload).

## `luban completion fish`

```fish
luban completion fish > ~/.config/fish/completions/luban.fish
```

Fish auto-loads anything in `completions/` next time you start a shell.

## `luban completion powershell`

```powershell
luban completion powershell > $PROFILE.luban.ps1
# add to $PROFILE once
. $PROFILE.luban.ps1
```

Or paste the output directly into your `$PROFILE`. Works in PowerShell 5.1 and 7+.

## How the scripts are generated

Every script is a **static string literal** baked into `luban.exe`, sharing a single source-of-truth verb list (`kVerbsSpaceSep` in `src/commands/completion.cpp`). They do not introspect the CLI registry at runtime — we maintain them by hand alongside new verbs. This keeps `luban completion <shell>` cheap and offline-only.

Trade-off: if a verb is added without updating `src/commands/completion.cpp`, the new verb still works on the command line but won't tab-complete until the next release. Acceptable: the canonical CLI surface is small (~17 verbs) and changes in lockstep with releases.
