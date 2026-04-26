# Why no IR, why `luban.cmake`

This page documents an alternative design path **we did not take** and why.

## The temptation: a luban IR

The first instinct when wrapping cmake + vcpkg + ninja is: "let's invent a unified frontend." User writes `luban.toml`, luban lowers it to cmake/vcpkg/ninja config files. xmake, meson, build2, premake all chose this path.

Sketch:

```toml
# luban.toml as primary manifest (REJECTED)
[package]
name = "myapp"
cpp = "23"

[build]
kind = "exe"
sources = ["src/**.cpp"]

[deps]
fmt = "10"
spdlog = "1.13"
```

→ luban generates `CMakeLists.txt` + `vcpkg.json` + `CMakePresets.json` from this.

## Why we rejected it

### 1. The user has to learn cmake **anyway**

C++ has decades of accumulated knowledge in cmake idioms. Custom commands, target_compile_options for one file, conditional features, link_options for memory sanitizer — every real-world project hits cases where the IR doesn't have a direct equivalent and the user has to know cmake to escape.

### 2. The escape hatch is hostile

If `luban.toml` is the source of truth and we *generate* `CMakeLists.txt`, then user edits to the generated cmake either:
- get clobbered on next regen (frustrating), or
- require a complex "eject" ritual (create-react-app-style, painful).

Either way the seam is sharp.

### 3. Re-implementing cmake is a tax we don't owe

vcpkg's manifest mode + cmake's preset system have been evolving for years. Inventing parallel concepts in luban means tracking their evolution forever — for marginal user value.

## What we did instead: the `include()` model

`luban.cmake` is a *standard cmake module*. The user's `CMakeLists.txt` does:

```cmake
cmake_minimum_required(VERSION 3.25)
project(foo CXX)

include(${CMAKE_SOURCE_DIR}/luban.cmake)
luban_register_targets()

# Below this line: standard cmake. Luban does not touch.
```

`luban.cmake` exposes two functions:

- `luban_apply(target)` — call once per target after `add_executable` / `add_library`. Sets cpp std, warnings, links vcpkg deps.
- `luban_register_targets()` — call once from root. Does `add_subdirectory(src/<name>)` for every entry in `LUBAN_TARGETS`.

**That's it.** Two functions, minimal API surface, no DSL.

## Properties of this design

| Property | How it works |
|---|---|
| User can edit `CMakeLists.txt` freely | Luban only writes `luban.cmake`; the CMakeLists.txt is theirs from day 1 (after `luban new`). |
| Project builds without luban | `cmake --preset default` works on any machine with cmake + vcpkg. `luban.cmake` is just a regular include. |
| User can opt out at any time | Delete the `include(luban.cmake)` and `luban_register_targets()` lines. The project becomes plain cmake. |
| Luban can update its codegen without breaking your project | We rewrite `luban.cmake` on each `luban add/sync`, but never touch the user's `CMakeLists.txt` after `luban new`. |
| Per-target customization is plain cmake | User writes `target_compile_options(foo PRIVATE -O3)` in `src/foo/CMakeLists.txt` after `luban_apply(foo)`. No Luban DSL needed. |

## What `luban_apply()` actually does

For a project with `vcpkg.json` declaring `["fmt", "spdlog"]` and no luban.toml:

```cmake
function(luban_apply target)
    target_compile_features(${target} PRIVATE cxx_std_23)
    if(NOT MSVC)
        target_compile_options(${target} PRIVATE -Wall -Wextra)
        target_link_options(${target} PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
    target_link_libraries(${target} PRIVATE
        fmt::fmt
        spdlog::spdlog
    )
endfunction()
```

Three concerns: language standard, warning policy, vcpkg deps. All three are derived from `vcpkg.json` + `luban.toml` (with sensible defaults). All three are things you'd write in cmake by hand anyway.

## What luban does NOT generate

- `CMakeLists.txt` (root and per-target — user-owned after `luban new`)
- `CMakePresets.json` (user-owned after `luban new`)
- `vcpkg.json` (vcpkg's manifest — luban only edits via `add`/`remove`)
- Source files
- Test files

All four are written exactly once by `luban new`, and after that they belong to the user.
