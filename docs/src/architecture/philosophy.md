# Design philosophy

Luban is shaped by **6 hard rules** that came out of the design discussions. They constrain a lot of seemingly-arbitrary decisions in the code; if you find a piece of luban surprising, the answer is probably here.

## 1. Auxiliary, not authoritative

> "luban is the helper. cmake / vcpkg / ninja are the main characters."

Luban does not invent a new build system. It does not invent a new IR. It does not invent a new manifest format. cmake remains the C++ build de facto, vcpkg remains the package source. Luban writes a `luban.cmake` module; one `include(luban.cmake)` line in your `CMakeLists.txt` brings it in, one line removed and Luban is gone.

**Concretely**: the moment a user wants to do something Luban doesn't directly support, they should be able to fall through to plain cmake / vcpkg without any "ejection" ritual.

## 2. No new manifest format

Deps live in `vcpkg.json` (vcpkg's own schema). `luban add fmt` edits `vcpkg.json` directly — there is no `[dependencies]` table in `luban.toml` parallel to vcpkg's manifest. **One source of truth per concern.**

`luban.toml` is **optional** and only holds *project-level luban preferences*: warning level, sanitizers, default preset, triplet. If you have no preferences, `luban.toml` does not exist.

## 3. `luban.cmake` is git-tracked

The whole point of "luban as auxiliary" is that the project must remain buildable on a machine that doesn't have luban installed. `luban.cmake` is a standard cmake module — committed to git, always reproducible, no luban needed at consumer-end.

This is the opposite of an `eject` model: the generated artifacts are first-class, not staging.

## 4. XDG-first paths, even on Windows

Linux's XDG Base Directory spec is the cleanest model the field has produced for "where do tools put their stuff." We respect `XDG_DATA_HOME`, `XDG_CACHE_HOME`, `XDG_STATE_HOME`, `XDG_CONFIG_HOME` even on Windows. Only when those are absent do we fall back to `%LOCALAPPDATA%` / `%APPDATA%` defaults.

**Why on Windows too**: it makes container/CI/multi-user scenarios trivial (just set `LUBAN_PREFIX=/somewhere`), and a future Linux port has no surprises.

See [reference/paths.md](../reference/paths.md) for the full resolution order.

## 5. Zero UAC

Every Luban-managed file lives in user-writable directories. We never write `Program Files`, never touch `HKLM`, never spawn an installer that needs admin. This is the same instinct as `rustup`, `pipx`, `volta` — the user owns their environment.

**Concretely**: `luban env --user` writes to `HKCU\Environment` (current user only). Toolchains land in `~/.local/share/luban/`. Nothing requires `runas`.

## 6. Single static binary

`luban.exe` is one file. Static-linked libc++, no DLLs to ship alongside, no Python runtime, no MSI. You can drop it on a USB stick, copy it to a fresh VM, run it on a Windows-on-ARM laptop someday — it just works (assuming Windows 10+).

**Concretely**: 31 MB binary. ~14 MB of that is luban code; ~10 MB is vendored miniz + nlohmann_json + toml++; ~7 MB is statically-linked libc++ + libwinpthread + libunwind.

---

## What this rules out

- **No luban-only DSL** for build description (xmake/meson territory)
- **No proprietary lockfile format** (vcpkg has its own, we use it)
- **No global system PATH writes**, no admin install
- **No Python or other runtime** dependency
- **No "luban world" you have to live inside** — every artifact is something cmake / vcpkg / git can already consume

## What this admits

- Light, narrow scope. Luban does **toolchain bootstrap** + **cmake/vcpkg glue** + nothing else.
- A `luban.exe` upgrade is an `.exe` swap, no migration needed.
- A future Linux port is plausible without rewriting; the philosophy already accommodates it.

## Cross-references

- [Two-tier dependency model](./two-tier-deps.md) — why system tools and project libs are separate concerns
- [Why no IR, why luban.cmake](./why-cmake-module.md) — the technical alternative we considered and rejected
- [Roadmap](./roadmap.md) — what's next
