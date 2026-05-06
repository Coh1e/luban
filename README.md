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

### Blueprint schema (v0.2.0)

```toml
schema = 1
name = "my-bp"

[tool.ripgrep]                          # one tool entry per binary you want on PATH
source = "github:BurntSushi/ripgrep"

[config.git]                            # render a config file via the git renderer
userName = "alice"

[file."~/.config/starship.toml"]        # drop a literal file
mode = "merge"                          # replace | drop-in | merge | append
content = '{"add_newline": false}'

[meta]
requires = ["main/foundation"]          # enforced — apply order is explicit
```

Singular keys (`tool` / `config` / `file`) — the v0.2.0 schema rename
(议题 P) replaced the old plural form; old bps need to flip.

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
