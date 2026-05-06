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
| `LUBAN_PARALLEL_CHUNKS` | `4` | bp apply Range download concurrency (0 = single-stream, max 16). **If GitHub CDN throttles, drop to 1-2 for faster effective speed.** |
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

```pwsh
# 1. Register the foundation blueprint source (one-time)
luban bp src add Coh1e/luban-bps --name main

# 2. Build the workshop (toolchain + CLI utilities)
luban bp apply main/cpp-base
luban bp apply main/cli-base
luban env --user                       # register HKCU PATH; new shells just work

# 3. Create + build a project
luban new app hello && cd hello
luban add fmt && luban build
```

The luban binary embeds **zero blueprints**. The foundation set
(`cpp-base` / `cli-base` / `git-base` / `onboarding`) lives in
[Coh1e/luban-bps](https://github.com/Coh1e/luban-bps). Anyone can publish
their own blueprint source repo and `luban bp src add` it.

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
