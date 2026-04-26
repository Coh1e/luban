# Commands overview

Luban has 15 commands, organized into four groups.

## Toolchain & environment (one-time per machine)

| Command | What it does |
|---|---|
| [`luban setup`](./setup.md) | Install LLVM-MinGW + cmake + ninja + mingit + vcpkg into `<data>/toolchains/` |
| [`luban env`](./env.md) | Show env state; rewrite activate scripts; **register HKCU PATH (rustup-style)** |

## Per-project (run inside a project dir)

| Command | What it does |
|---|---|
| [`luban new app|lib <name>`](./new.md) | Scaffold a new project (CMakeLists + vcpkg.json + luban.cmake + src/) |
| [`luban build`](./build.md) | `cmake --preset && cmake --build`; sync `compile_commands.json` |
| [`luban target add|remove`](./target.md) | Add/remove a build target (lib or exe) within the project |

## Dependency management (vcpkg.json + luban.cmake)

| Command | What it does |
|---|---|
| [`luban add <pkg>[@version]`](./add.md) | Edit `vcpkg.json` + regenerate `luban.cmake` (find_package + link auto-wired) |
| [`luban remove <pkg>`](./remove.md) | Reverse `luban add` |
| [`luban sync`](./sync.md) | Re-read `vcpkg.json` + `luban.toml`, regenerate `luban.cmake` |
| [`luban search <pattern>`](./search.md) | Search vcpkg ports (wraps `vcpkg search`) |

## Advanced / diagnostic

| Command | What it does |
|---|---|
| [`luban doctor`](./doctor.md) | Report directories, installed components, tools on PATH |
| [`luban run <cmd> [args...]`](./run.md) | uv-style activate + exec; runs cmd with luban env |
| [`luban which <alias>`](./which.md) | Print absolute exe path that an alias resolves to |
| [`luban describe [--json]`](./describe.md) | Dump system + project state for IDEs / scripts |
| [`luban shim`](./shim.md) | Regenerate `<data>/bin/` shims (text + .exe; repair tool) |

## Global flags

These work before any subcommand:

| Flag | Effect |
|---|---|
| `-V`, `--version` | Print `luban X.Y.Z` and exit |
| `-h`, `--help` | Print top-level help |
| `-v`, `--verbose` | Verbose log output (including stack traces on internal errors) |

## Conventions

- **Idempotent**: every command can be re-run safely. `luban setup` skips already-installed components, `luban add` replaces existing dep, `luban target add` refuses duplicate names.
- **Atomic file writes**: every config/manifest write goes via `tmp + rename`, so a crash leaves either the old or new file fully intact, never half-written.
- **Idiomatic exit codes**: `0` success, `1` runtime failure (download failed, cmake error), `2` user error (invalid args, refused operation).
- **Logs to stderr**: useful info (`✓`, `→`, `!`, `✗` prefixed lines) goes to **stderr**. **stdout** is for machine-readable output (e.g., `compile_commands.json` paths). You can pipe stdout cleanly.
