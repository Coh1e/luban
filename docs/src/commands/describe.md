# `luban describe`

Dump system + (optional) project state. Default human-readable; `--json` for
machines.

## Synopsis

```text
luban describe [--json]
```

## Default output (text)

```text
Luban 0.1.0

── Paths ──
  bin                 C:\Users\you\.local\share\luban\bin
  cache               C:\Users\you\.cache\luban
  config              C:\Users\you\.config\luban
  data                C:\Users\you\.local\share\luban
  ...

── Installed components (5) ──
  cmake 4.3.2  →  C:\Users\you\.local\share\luban\toolchains\cmake-4.3.2-x86_64
      5 alias(es), source: scoop-main
  llvm-mingw 20260421  →  ...
      270 alias(es), source: overlay
  ...

── Project: foo 0.1.0 ──     (only when run inside a luban project)
  root: C:\Users\you\projects\foo
  deps (2):  fmt  spdlog@>=1.13
  luban.toml: cpp=23 triplet=x64-mingw-static warnings=normal sanitizers=[]
  builds:  default(✓)  release(—)
```

The "Project" section only appears when run from inside a directory tree
containing `luban.cmake` or `vcpkg.json`.

## `--json` output (machine-readable)

```json
{
  "luban_version": "0.1.0",
  "paths": { "bin": "...", "cache": "...", ... },
  "installed_components": [
    {
      "name": "cmake",
      "version": "4.3.2",
      "source": "scoop-main",
      "url": "https://...",
      "hash": "sha256:...",
      "toolchain_dir": "C:\\Users\\...\\cmake-4.3.2-x86_64",
      "architecture": "x86_64",
      "installed_at": "2026-04-25T...",
      "bins": [
        {
          "alias": "cmake",
          "relative_path": "bin/cmake.exe",
          "absolute_path": "C:\\Users\\...\\cmake.exe"
        },
        ...
      ]
    },
    ...
  ],
  "project": {           // when in a project
    "root": "...",
    "name": "foo",
    "version": "0.1.0",
    "dependencies": [
      { "name": "fmt" },
      { "name": "spdlog", "version_ge": "1.13.0" }
    ],
    "luban_toml": { "cpp": 23, "triplet": "...", "warnings": "...", ... },
    "builds": [
      { "preset": "default", "dir": "...", "compile_commands": true }
    ],
    "compile_commands_root": "..."
  }
}
```

Schema is stable for v0.x. Changes will be flagged in release notes.

## Use cases

- **IDE plugin backend** — Neovim/VS Code/CLion plugins can shell out to
  `luban describe --json` to discover toolchain paths, deps, and build state
  in one call.
- **Visualization** — pipe to D3.js / vis.js to render a system+project graph
  in a browser (no need to compile luban to WASM; the JSON is the API).
- **Debugging** — "is fmt actually in this project's deps? what version does
  vcpkg.json say?" → `luban describe --json | jq .project.dependencies`
- **Scripting** — `luban describe --json | jq -r '.installed_components[].name'`
  to enumerate components in shell scripts.

## Examples

```bat
luban describe                          :: human-readable
luban describe --json                   :: full JSON dump
luban describe --json | jq .paths       :: just paths
luban describe --json | jq '.installed_components[] | {name, version}'
:: extract just name + version from each component
```
