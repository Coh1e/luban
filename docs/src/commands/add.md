# `luban add`

Add a vcpkg library to the current project. **Edits `vcpkg.json`** + **regenerates `luban.cmake`** (find_package + target_link_libraries auto-wired).

## Synopsis

```text
luban add <pkg>[@<version>]
```

`<pkg>` is a [vcpkg port name](https://vcpkg.io/en/packages.html). `<version>` is optional; when given, becomes a `version>=` constraint in `vcpkg.json`.

## Examples

```bat
luban add fmt                      # → vcpkg.json: ["fmt"]
luban add spdlog@1.13              # → version>=: 1.13.0
luban add boost-asio               # Boost.Asio (Boost::asio target)
luban add catch2                   # link target: Catch2::Catch2WithMain
```

## What `luban add fmt` actually does

Before:
```jsonc
// vcpkg.json
{ "name": "myapp", "version": "0.1.0", "dependencies": [] }
```

After:
```jsonc
// vcpkg.json
{
  "name": "myapp",
  "version": "0.1.0",
  "dependencies": ["fmt"]
}
```

And `luban.cmake` is regenerated to include:

```cmake
find_package(fmt CONFIG REQUIRED)

function(luban_apply target)
    target_compile_features(${target} PRIVATE cxx_std_23)
    if(NOT MSVC)
        target_compile_options(${target} PRIVATE -Wall -Wextra)
        target_link_options(${target} PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
    target_link_libraries(${target} PRIVATE
        fmt::fmt
    )
endfunction()
```

The `find_package(fmt ...)` line is unconditional. The `target_link_libraries(... fmt::fmt)` line is added inside `luban_apply()` so every target that calls `luban_apply()` automatically links fmt.

## Version constraints

| Spec | vcpkg.json result |
|---|---|
| `luban add fmt` | `"fmt"` (string form, baseline version) |
| `luban add fmt@10` | `{"name":"fmt","version>=":"10.0.0"}` |
| `luban add fmt@10.2` | `{"name":"fmt","version>=":"10.2.0"}` |
| `luban add fmt@10.2.1` | `{"name":"fmt","version>=":"10.2.1"}` |

Version padding to 3 segments is automatic — vcpkg expects `X.Y.Z` for `version>=`. Suffixes (e.g., `1.0.0-alpha`) pass through unchanged.

To **pin** an exact version (not just a lower bound), edit `vcpkg.json` manually after `luban add`:

```jsonc
{
  "dependencies": [{"name": "fmt", "version>=": "10.2.0"}],
  "overrides": [{"name": "fmt", "version": "10.2.0"}]
}
```

Then run `luban sync` to regenerate `luban.cmake` (no functional change but confirms the schema is parsed).

## System tools are rejected

```bat
$ luban add cmake
✗ 'cmake' is a system-level toolchain, not a vcpkg library.
· System tools live in <data>/toolchains/. Use:
·   luban setup --only cmake
```

This is enforced for: `cmake`, `ninja`, `clang`, `clang++`, `clangd`, `clang-format`, `clang-tidy`, `lld`, `llvm-mingw`, `mingit`, `git`, `vcpkg`, `make`, `gcc`. See [Two-tier dependency model](../architecture/two-tier-deps.md) for why.

## When the cmake target name is wrong

`luban add` uses a curated `vcpkg port → cmake target` mapping (~50 popular libs). For a library not in the table, luban falls back to `find_package(<port> CONFIG REQUIRED)` and **does not** auto-link it. You'll need to:

1. Look up the cmake target name in vcpkg's [usage file](https://vcpkg.io/en/packages.html) (after first `luban build` they're in `vcpkg_installed/<triplet>/share/<pkg>/usage`)
2. Add `target_link_libraries(<your-target> PRIVATE <Mod>::<target>)` in your `src/<target>/CMakeLists.txt`

Future luban will auto-discover from usage files; in M2.5 it's manual fallback.

## Does it run vcpkg install?

**No.** `luban add` only edits `vcpkg.json`. The actual fetching + building of fmt happens later, when `cmake` runs (because `CMakePresets.json` points `CMAKE_TOOLCHAIN_FILE` at vcpkg's, and vcpkg in manifest mode auto-installs deps during cmake configure).

So the sequence is:

```bat
luban add fmt        # fast: just file edits
luban build          # slow first time: cmake → vcpkg fetches + builds fmt → links
luban build          # subsequent: vcpkg cache hit, just compile your code
```

Future flag idea: `luban add fmt --install` for "fetch right now". Not in v1.

## See also

- [`luban remove`](./remove.md) — symmetric reverse
- [`luban sync`](./sync.md) — regenerate `luban.cmake` without changing deps
- [Reference → `luban.cmake`](../reference/luban-cmake.md) — what gets generated and why
- [vcpkg manifest mode docs](https://learn.microsoft.com/en-us/vcpkg/users/manifests)
