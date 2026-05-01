# Contributing

## Building luban from source

```bat
git clone https://github.com/Coh1e/luban.git
cd luban

:: requires LLVM-MinGW + cmake + ninja already on PATH
:: easiest: install luban first, then `luban env --user`, then build:
cmake --preset default
cmake --build --preset default
:: build/default/luban.exe
```

## Repo layout

```
src/                       # luban's C++23 source
  cli.{hpp,cpp}            # subcommand dispatch, help renderer
  paths.{hpp,cpp}          # XDG-first path resolution (Win32 SHGetKnownFolderPath fallback)
  registry.{hpp,cpp}       # installed.json schema (1:1 with Python predecessor)
  scoop_manifest.{hpp,cpp} # Scoop manifest parser with safety whitelist
  vcpkg_manifest.{hpp,cpp} # vcpkg.json reader/writer
  luban_toml.{hpp,cpp}     # luban.toml schema v1
  luban_cmake_gen.{hpp,cpp}# luban.cmake generator
  lib_targets.{hpp,cpp}    # curated vcpkg port → cmake target mapping
  download.{hpp,cpp}       # WinHTTP wrapper with retry + progress
  hash.{hpp,cpp}           # BCrypt SHA256/SHA512 file hashing
  archive.{hpp,cpp}        # miniz ZIP extract with path-traversal guard
  env_snapshot.{hpp,cpp}   # PATH + LUBAN_* compute
  shim.{hpp,cpp}           # .cmd / .ps1 / sh shim writer
  proc.{hpp,cpp}           # CreateProcessW wrapper
  win_path.{hpp,cpp}       # HKCU PATH + env var registry ops
  bucket_sync.{hpp,cpp}    # Scoop manifest fetcher
  selection.{hpp,cpp}      # selection.json reader + seed deployment
  component.{hpp,cpp}      # full install pipeline
  log.{hpp,cpp}            # ANSI logger
  commands/                # one cpp per CLI verb
    doctor.cpp env.cpp setup.cpp shim_cmd.cpp
    new_project.cpp build_project.cpp
    add.cpp target_cmd.cpp
  util/win.hpp             # utf8 ↔ wstring conversions

third_party/               # vendored single-header libs
  json.hpp                 # nlohmann/json, MIT
  miniz.{h,c}              # Rich Geldreich, BSD-3
  toml.hpp                 # marzer/tomlplusplus, MIT
  *.LICENSE

manifests_seed/            # default selection + overlay manifests
  selection.json
  llvm-mingw.json mingit.json vcpkg.json

templates/                 # `luban new` scaffolding
  app/

docs/                      # this site (mdBook + Doxygen)
  src/                     # mdBook source
  Doxyfile                 # Doxygen config
  doxygen-awesome*.{css,js}# vendored modern theme
  book.toml                # mdBook config

.github/workflows/         # CI: docs build + (future) release
```

## Coding conventions

- **C++23**, `clang++` from LLVM-MinGW. `-Wall -Wextra -Wpedantic`.
- **Static link** in release builds (`-static -static-libgcc -static-libstdc++`).
- **Single binary**: every dep is vendored single-header in `third_party/`. No CMake `find_package` calls in luban's own build.
- **Win32 native**: prefer `CreateProcessW` / `WinHttpSendRequest` / `BCryptHashData` / `RegSetValueExW` over libc abstractions, where stable behavior matters.
- **No dynamic allocation in hot paths**. We're a CLI; one-shot allocation patterns are fine.
- **Descriptive comments where the code is non-obvious**; see existing files for style.

## Testing

Two layers, both run in CI:

- **Unit tests** (doctest, ADR-0004) — `tests/test_<module>.cpp` per leaf module
  (currently `hash`, `lib_targets`, `luban_toml`, `marker_block`). Build with
  `cmake --build --preset release --target luban-tests` then run
  `./build/release/luban-tests.exe`. ~40 cases / ~110 assertions.
- **End-to-end smoke** — `scripts/smoke.bat` exercises `new → add/remove → build →
  run → specs → target add/build/remove → doctor`. Zero vcpkg network dependency.

Add new tests in the `LUBAN_BUILD_TESTS` block in `CMakeLists.txt`. Pure-compute
modules (paths, scoop_manifest, archive) are good candidates for the next round.

## Where to start

Look for issues tagged `good-first-issue`. Generally welcome:
- Curating more `lib_targets` mappings (`src/lib_targets.cpp`)
- More overlay manifests in `manifests_seed/` (e.g., for tools that need an installer.script bypass)
- Documentation improvements (this site is in `docs/src/`)
