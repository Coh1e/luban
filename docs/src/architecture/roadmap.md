# Roadmap

This is the rough order in which features land. Anything past M2.5 is open territory.

## ✅ M1 — User-facing commands (done)

C++ ports of the Python bootstrap commands: `doctor`, `env`, `new`, `build`, `shim`. Single static `luban.exe`.

## ✅ M2 — Setup pipeline ported (done)

`luban setup` end-to-end in C++:
- WinHTTP downloads, BCrypt SHA verification, miniz ZIP extract
- Scoop manifest parsing (with safety whitelist)
- Component install: download → verify → extract → shim → registry
- Bucket sync (per-manifest fetch from raw.githubusercontent)
- vcpkg overlay manifest + `bootstrap-vcpkg.bat` integration

Python bootstrap retired.

## ✅ M2.5 — cmake/vcpkg auxiliary frontend (done)

The "user-facing" half of luban's value:
- `luban add` / `luban remove` / `luban sync` (vcpkg.json + luban.cmake)
- `luban target add` / `luban target remove` (multi-target)
- `luban.cmake` generator (find_package, luban_apply, luban_register_targets)
- `luban.toml` schema v1 ([project], [scaffold])
- Curated pkg → cmake target mapping (started at ~50, now ~224)
- Scaffold improvements: subdir-from-day-1, vcpkg-configuration.json baseline
- `luban env --user` extended to set HKCU env vars (LUBAN_*, VCPKG_ROOT)

## ✅ M3 — Daily-driver polish (shipped, v0.2/v0.3)

| Feature | Notes |
|---|---|
| `luban search <pattern>` | Wraps `vcpkg search`; adds caching |
| `luban which <alias>` | Show absolute exe path for a registry alias |
| `luban describe --json` | Machine-readable project + system state for IDE plugins |
| `luban run <cmd> [args...]` | uv-style transparent activation + exec |
| Real `.exe` shim | rustup-style native exe proxies alongside `.cmd` shims |
| Shell installer | `irm https://luban.coh1e.com/install.ps1 \| iex` (uv-style); replaces the previously-planned `luban-init.exe` (rejected: luban.exe is already a single self-contained binary, see ADR-0001 alt D) |
| `luban doc` | Doxygen wrapper, cargo doc-style |
| `luban specs` | SAGE / AGENTS.md scaffolding for AI agents (ADR-0003) |
| `luban completion <shell>` | clink + bash + zsh + fish + powershell |
| `luban env --msvc-init` | vswhere + vcvarsall env capture; HKCU writeback in Phase 2 |
| `luban doctor --strict --json` | CI-gateable strict mode + machine-readable JSON |
| Per-project toolchain pin | `luban.toml [toolchain]` map, `luban build` warns on mismatch (Phase 1) |
| Chunked Range download | 4-thread parallel HTTP downloads in component install |
| Unit tests | doctest vendored (ADR-0004); 40 cases / 110 assertions |
| Multi-target build | `luban new --target=wasm` (emscripten path) |

## 🌅 M4+ — Beyond the immediate need

| Theme | Examples |
|---|---|
| Workspace support | `luban.toml [workspace] members = [...]`, shared build cache |
| Toolchain pin per project | ✅ Phase 1 done (v0.3) — `luban.toml [toolchain]` map, `luban build` warns on mismatch. Phase 2: `--strict-pins` flag for CI. |
| Linux/macOS port | Design frozen — see ADR-0006. Phase A: build clean on POSIX; Phase B: hash via OpenSSL + download via libcurl; Phase C: `~/.bashrc` PATH writes, symlink shims, platform triplets. |
| luban registry (OQ-7) | Design frozen — see ADR-0005. vcpkg-complementary; categories: lib, lib-rs (Rust FFI via cxx-bridge), template, toolchain (manifests_seed migrates here). |
| TUI / interactive mode | `luban setup -i`, FTXUI; first-run wizard |
| Visualization (`/luban describe --view`) | dump JSON, open static HTML page with D3.js graph |
| `luban tool install <pkg>` | pipx-equivalent for global C++ CLI tools |

## Out of scope (for now)

- Replacing cmake or vcpkg with anything custom
- Cross-machine sync, "luban cloud", telemetry
- IDE plugins maintained by us (we generate compile_commands.json + standard cmake; let editors own integration)
- Building luban itself with anything other than LLVM-MinGW (MSVC support might come if asked, but not a priority)
