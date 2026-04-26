# Two-tier dependency model

> "Luban also needs to do system bin management. cmake doesn't have to be locked inside luban — like Python where I `pip install` for system tools or for project libs."
> — the conversation that sparked this design

Luban distinguishes two kinds of "dependency" that the user sees, and treats them as **categorically different concerns**:

| Layer | Examples | Stored in | Scope |
|---|---|---|---|
| **System** | cmake, ninja, clang, clangd, lld, git, vcpkg itself | `<data>/toolchains/<name>-<ver>-<arch>/` | Cross-project, machine-wide (one user) |
| **Project** | fmt, spdlog, boost, catch2, nlohmann-json | `<project>/vcpkg_installed/<triplet>/` | Per-project, in-tree |

This is the same split that mature ecosystems converge on:

| Ecosystem | System tier | Project tier |
|---|---|---|
| Rust | `rustup` | `cargo` |
| Python | `pipx` / `uv tool` | `uv` / `pip` (in venv) |
| Node | `volta` / `nvm` | `npm` / `pnpm` |
| Go | system go | `go.mod` |
| **C++ via Luban** | `luban setup` (LLVM-MinGW etc.) | `luban add` (vcpkg deps) |

## Hard rules (the constraints we enforce)

1. **Toolchain names not allowed in `vcpkg.json`.** `luban add cmake` errors out and points the user to `luban setup`. Same for ninja, clang, git, etc.

2. **Project libs not allowed in `luban setup`.** If you `luban setup --only fmt`, it'll just fail to find an overlay manifest (and Scoop's `fmt` manifest, if any, is ports-via-installer-script which we refuse).

3. **One source of truth per layer.** System tier is `<state>/installed.json`. Project tier is `vcpkg.json`. Never duplicated, never conflated.

4. **System tier upgrades are global.** All projects on this machine see the same `cmake` version. (Future: per-project pin via `luban.toml [toolchain]`, but not in v1.)

5. **Project tier installs land per-project** (`vcpkg_installed/<triplet>/`). They are `.gitignore`d. Their reproducibility comes from the manifest+baseline, not the install.

## Why two layers and not one

It's tempting to ask: why doesn't `luban add cmake` just work? After all, cmake is a dep of the project too.

The answer is **failure-mode separation**:

- **Toolchain breakage**: rare, big consequence. A bad cmake version blocks ALL your projects. You want this rare, deliberate, version-controlled.
- **Library breakage**: common, small consequence. A bad `fmt` upgrade affects one project's link. You want this fast, throwaway.

Mixing them means a routine `luban add` could silently bump cmake. That's the kind of design that turns into a 3 AM incident.

## What luban does NOT have (yet)

These would round out the model but aren't in v1:

- **Per-project toolchain pin**: `luban.toml [toolchain] cmake = "4.3.2"`. Today the system tier is global.
- **Workspaces**: a single `luban.toml [workspace] members = ["foo", "bar"]` describing multiple sibling projects sharing build cache + vcpkg cache. Cargo-style.
- **Global tool install**: `luban tool install <pkg>` (pipx-equivalent) for command-line C++ tools (e.g., a static analyzer). Today only toolchains are system-level.

These are open M3+ territory.
