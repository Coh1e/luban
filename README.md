# Luban (鲁班)

[![license](https://img.shields.io/badge/license-MIT-blue)](LICENSE)
[![中文](https://img.shields.io/badge/中文-README-red)](README.zh-CN.md)

**Build a C++ workshop from a blueprint.**

luban reads a Lua/TOML blueprint and assembles your machine: toolchain
(LLVM-MinGW + cmake + ninja + mingit + vcpkg), CLI utilities (ripgrep / fd / bat / ...),
and dotfiles (git / bat / fastfetch / ...). Layer blueprints on top of each
other; roll back atomically.

Single static-linked binary, zero UAC, XDG-first directories.
Embeds Lua 5.4. Windows-first; POSIX builds.

Each release ships **two flavors**, both static-linked: `luban-msvc.exe`
(~3 MB, default; MSVC `/MT`) and `luban-mingw.exe` (~6 MB; LLVM-MinGW
`-static`). CI verifies invariant 7 per flavor (no vcruntime / msvcp /
libgcc_s / libstdc++ deps).

## Install

### Windows (PowerShell, recommended)

```pwsh
irm https://github.com/Coh1e/luban/raw/main/install.ps1 | iex
```

The installer drops `luban.exe` + `luban-shim.exe` into `~/.local/bin`
(shared with uv / pipx / claude-code), verifies SHA256 against
`SHA256SUMS` from the latest release, and prompts you to run
`luban env --user` (registers `~/.local/bin` and toolchain bins in HKCU PATH).
Re-running on an already-installed machine SHA-compares before re-downloading.

#### Tuning env vars

| env | default | what it does |
|---|---|---|
| `LUBAN_INSTALL_DIR` | `~/.local/bin` | installer destination |
| `LUBAN_FLAVOR` | `msvc` | which release binary (`msvc` or `mingw`) |
| `LUBAN_FORCE_REINSTALL` | unset | `=1` re-downloads even if SHA matches |
| `LUBAN_GITHUB_MIRROR_PREFIX` | unset | reverse-proxy prefix (e.g. `https://ghfast.top`) for slow CN/SEA networks; honored by both installer and luban itself |
| `LUBAN_PARALLEL_CHUNKS` | `1` | bp apply Range download concurrency (default single-stream; max 16). GitHub's release CDN per-IP-throttles parallel connections aggressively (1 → 4.7 MB/s, 4 → 150 KB/s aggregate on VN networks). Crank up only on private S3 / internal mirrors. |
| `LUBAN_PROGRESS` / `LUBAN_NO_PROGRESS` | unset | force progress bar on / off (auto-enabled on TTY) |

### Manual

Download the flavor you want from the
[latest release](https://github.com/Coh1e/luban/releases/latest)
and rename to `luban.exe` / `luban-shim.exe`:

```pwsh
gh release download --repo Coh1e/luban `
  -p luban-msvc.exe -p luban-shim-msvc.exe -p SHA256SUMS
sha256sum -c SHA256SUMS
Rename-Item luban-msvc.exe      luban.exe
Rename-Item luban-shim-msvc.exe luban-shim.exe
```

Drop both into a directory on PATH (e.g. `~/.local/bin`).

### Update / Uninstall

```pwsh
luban self update                              # fetch latest release, swap binary
luban self uninstall --yes                     # full cleanup (binary + dirs + env)
luban self uninstall --yes --keep-toolchains   # reset bp state, keep cmake/vcpkg
```

`self uninstall` is **ownership-gated**: HKCU env vars (VCPKG_ROOT,
EM_CONFIG, msvc captures, PATH entries) are unset only if their value
points inside luban's own dirs. External paths are preserved with a log
line — luban won't clobber a hand-rolled env.

## Quickstart

`install.ps1` already pre-applies `main/foundation` (git + ssh + lfs + gcm —
true prereq for almost everything else, no prompt) and prompts for
`main/cpp-toolchain`. After install:

```pwsh
# Optional extras the installer didn't apply for you
luban bp apply main/cli-tools          # zoxide / starship / fd / ripgrep
luban env --user                       # register HKCU PATH; new shells just work

# Create + build a project
luban new app hello && cd hello
luban add fmt && luban build
```

The luban binary embeds **zero blueprints**. The foundation set
(`foundation` / `cpp-toolchain` / `cli-tools` / `onboarding`) lives in
[Coh1e/luban-bps](https://github.com/Coh1e/luban-bps). Anyone can publish
their own blueprint source repo and `luban bp src add` it.

### Blueprint cookbook

Blueprints live in `<bp-source>/blueprints/<name>.{toml,lua}`. Singular
TOML keys (`tool` / `config` / `file`) — the v0.2.0 schema (议题 P).
Examples below are TOML; the Lua form is in `Coh1e/luban-bps`'s
`onboarding.lua`.

#### 1. The simplest bp — drop a tool on PATH

```toml
schema = 1
name = "ripgrep"
[tool.ripgrep]
source = "github:BurntSushi/ripgrep"
```

`luban bp apply <src>/ripgrep` → fetch latest release zip → extract → shim
`rg.exe` under `~/.local/bin/`. The asset scorer picks the right zip by
host triplet; sha256 gets pinned into `<src>/blueprints/ripgrep.toml.lock`
on first apply.

#### 2. Multi-binary tool — explicit shim list

```toml
[tool.openssh]
source = "github:PowerShell/Win32-OpenSSH"
bin = "ssh.exe"
shims = ["ssh.exe", "ssh-keygen.exe", "ssh-agent.exe", "scp.exe", "sftp.exe"]
external_skip = "ssh.exe"      # if ssh is already on PATH (System32\OpenSSH), skip
```

`shim_dir = "bin"` is the auto-discover variant — every `*.exe` under
`bin/` gets a shim (used by `cpp-toolchain` for llvm-mingw's ~270
binaries; `shims` would go stale across upstream releases).

#### 3. Tool that bootstraps itself — `post_install` script

```toml
[tool.vcpkg]
source = "github:microsoft/vcpkg"      # source-zip fallback (no release artifacts)
bin = "vcpkg.exe"
post_install = "bootstrap-vcpkg.bat"   # path inside the extracted artifact
```

post_install runs once on fresh extraction (cwd = artifact root, on
Windows wrapped via `cmd /c`). vcpkg ships its own bootstrap script so
this works out of the box. For tools whose upstream zip ships *only*
the payload (no install logic — e.g. font files), use `bp:` prefix:

```toml
[tool.maple-mono]
source = "github:subframe7536/maple-font"
no_shim = true                          # not a CLI binary; nothing for PATH
post_install = "bp:scripts/register-fonts.ps1"
# bp:<rel> resolves against the bp-source-repo root, not the artifact.
# The script lives in YOUR bp source's scripts/, not the upstream zip.
```

Real bp: `Coh1e/luban-bps/blueprints/fonts.toml`. Registers all `.ttf`
in the artifact under HKCU\…\Fonts + AddFontResourceEx (per-user, no UAC).

#### 4. Configure a tool — render a dotfile

```toml
[config.git]                            # uses the built-in `git` renderer
userName = "alice"
userEmail = "alice@example.com"
lfs = true                              # adds [filter "lfs"] block
[config.git.aliases]
co = "checkout"
br = "branch"
```

Built-in renderers: `git` / `bat` / `fastfetch` / `yazi` / `delta`.
Output drops to `~/.gitconfig.d/<bp>.gitconfig` (drop-in mode); user
`[include]`s the dir from their main `~/.gitconfig` once. Custom
renderers via Lua blueprints land in v0.3.0 (议题 M(d)).

#### 5. Drop a literal config file (4 modes)

```toml
# (a) replace: overwrite target, back up original on first apply
[file."~/.config/starship.toml"]
mode = "replace"
content = '''
add_newline = false
[character]
success_symbol = "[❯](bold green)"
'''

# (b) drop-in: write to <canonical>.d/<bp> alongside the user's main file
[file."~/.gitconfig.d/work-aliases.gitconfig"]
mode = "drop-in"
content = "[alias]\n    co = checkout\n"

# (c) merge: JSON Merge Patch (RFC 7396) — preserve untouched keys,
#     update only what you specify. Use case: WT settings.json themes
#     section without clobbering the rest of the file.
[file."~/AppData/Local/Packages/Microsoft.WindowsTerminal_8wekyb3d8bbwe/LocalState/settings.json"]
mode = "merge"
content = '''
{
  "profiles": {
    "defaults": {
      "font": { "face": "Maple Mono NF CN", "size": 11 }
    }
  }
}
'''

# (d) append: bracket content in a luban marker block keyed by bp name —
#     re-applies replace the block in place; idempotent. Multi-bp
#     coordination on shared files like profile.ps1 just works.
[file."~/Documents/PowerShell/Microsoft.PowerShell_profile.ps1"]
mode = "append"
content = '''
Invoke-Expression (& { (zoxide init powershell | Out-String) })
Invoke-Expression (&starship init powershell)
'''
```

#### 6. Layered bps — `meta.requires`

```toml
[meta]
requires = ["main/foundation", "main/cli-tools"]   # enforced at apply time
conflicts = ["main/legacy-cpp-base"]               # advisory (planned)
```

If `main/foundation` isn't in the current generation, `bp apply` fails
with the exact `luban bp apply main/foundation` line you need to run
first. install.ps1's two-phase bootstrap (foundation no-prompt,
cpp-toolchain prompted) keeps the gate satisfied for new boxes.

#### 7. Personal onboarding bp pattern

A common shape for one-shot machine setup:

```toml
schema = 1
name = "onboarding"
description = "Win11 personal setup"

# Pull tools that don't live in the foundation layers
[tool.gh]
source = "github:cli/cli"
[tool.lazygit]
source = "github:jesseduffield/lazygit"

# Configure things foundation already installed
[config.git]
userName = "Coh1e"
userEmail = "you@example.com"
[config.git.credential]
helper = "manager"

# Drop your dotfiles
[file."~/.config/starship.toml"]
mode = "replace"
content = '''...'''

[file."~/Documents/PowerShell/Microsoft.PowerShell_profile.ps1"]
mode = "append"
content = '''
Set-Alias ll Get-ChildItem -Force
Invoke-Expression (&starship init powershell)
'''

[meta]
requires = ["main/foundation", "main/cli-tools", "main/fonts"]
```

Save under your own bp source repo (e.g. `~/dotfiles-bp/blueprints/onboarding.toml`),
register + apply:

```pwsh
luban bp src add D:\dotfiles-bp --name me
luban bp apply me/onboarding
```

A new machine: `irm install.ps1 | iex` → installer auto-applies foundation
+ prompts cpp-toolchain → `luban bp apply main/cli-tools main/fonts me/onboarding`
and you're back. ~3 minutes if the network behaves.

#### 8. PowerShell modules — `pwsh-module:` source scheme (v0.4.1+)

Tools that ship as PowerShell modules only (PSReadLine, PSFzf, posh-git,
…) get a first-class scheme. The .nupkg is just a zip; luban's existing
extract handles it. The bp ships a `post_install` script that copies
the module files into `~/Documents/PowerShell/Modules/<Name>/<Version>/`
where pwsh's `$PSModulePath` auto-discovers them.

```toml
[tool.psreadline]
source = "pwsh-module:PSReadLine"
version = "2.4.0"           # required (auto-latest = v0.4.x followup)
no_shim = true              # PowerShell modules go to PSModulePath, not PATH
bin = "PSReadLine.psd1"
post_install = "bp:scripts/install-pwsh-module.ps1"
```

Working example: `Coh1e/luban-bps/blueprints/pwsh-modules.toml` +
`scripts/install-pwsh-module.ps1`. The script is generic — reads name +
version from the `.psd1` manifest, so the same script works for any
module. Per-user, no UAC.

#### 9. Lua bps — `register_renderer` / `register_resolver` (v0.4.0 / v0.4.2)

When you need a config renderer that isn't one of the 5 built-ins
(`git` / `bat` / `fastfetch` / `yazi` / `delta`), or a source scheme
that isn't `github:` / `pwsh-module:`, write the bp in Lua. Tier 1
(DESIGN §9.9) lets a bp declare both inline:

```lua
-- onboarding.lua

-- Register a custom renderer for [config.starship]
luban.register_renderer("starship", {
  target_path = function(cfg, ctx)
    return ctx.xdg_config .. "/starship.toml"
  end,
  render = function(cfg, ctx)
    return string.format(
      "add_newline = %s\ncommand_timeout = %d\n",
      tostring(cfg.add_newline or false),
      cfg.command_timeout or 1000)
  end,
})

-- Register a custom source scheme `emsdk:` for emscripten releases
-- (hosted on Google Storage, not GitHub).
luban.register_resolver("emsdk", function(spec)
  return {
    url = string.format(
      "https://storage.googleapis.com/webassembly/emscripten-releases-builds/win/%s/wasm-binaries.zip",
      spec.version),
    sha256 = "sha256:<one-time-pin>",
    bin = "emscripten/emcc.bat",
  }
end)

return {
  schema = 1,
  name = "onboarding",
  tool = {
    emscripten = { source = "emsdk", version = "1724b50443d92e23ef2a56abf0dc501206839cef" },
  },
  config = {
    starship = { add_newline = false, command_timeout = 500 },  -- → custom renderer above
  },
}
```

What happens at apply:
1. luban parses the bp twice — first via a fresh engine for spec
   extraction, then via a long-lived engine where `register_*`
   side effects land in two registries.
2. Lock resolution consults the resolver registry FIRST (so `emsdk:`
   fires the bp's Lua fn, not a "unknown scheme" error).
3. Render phase consults the renderer registry FIRST (so
   `[config.starship]` fires the bp's renderer fn, not the embedded
   builtin fallback).

Both registries are per-apply scope; nothing leaks between bp applies.
TOML bps can't `register_*` (they have no Lua) but CAN `[config.X]` /
`source = "<scheme>:..."` against schemes / renderers a previously-
applied Lua bp registered (DESIGN §9.9 "同一注册表 / 无双码路径").

## Documentation

- **Design** (decided architecture) → [`docs/DESIGN.md`](./docs/DESIGN.md)
- **Future** (open questions, planned but unscheduled) → [`docs/FUTURE.md`](./docs/FUTURE.md)
- **AI agent guide** → [`CLAUDE.md`](./CLAUDE.md) + [`AGENTS.md`](./AGENTS.md)
- **Long-form `--help`** → `luban new --help`, `luban bp --help`, etc.

## License

Luban itself: [MIT](LICENSE). Vendored third-party:
- `third_party/json.hpp` — nlohmann/json, MIT
- `third_party/miniz.{h,c}` — Rich Geldreich, BSD-3-Clause (`third_party/miniz.LICENSE`)
- `third_party/toml.hpp` — Mark Gillard's toml++, MIT (`third_party/toml.LICENSE`)
- `third_party/doctest.h` — Viktor Kirilov, MIT (`third_party/doctest.LICENSE`)
- `third_party/lua54/` — Lua 5.4, MIT (`third_party/lua54/LICENSE`)
- `third_party/quickjs/` — QuickJS-NG, MIT
