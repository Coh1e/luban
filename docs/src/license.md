# License

## Luban itself

License: TBD (likely MIT or Apache-2.0).

## Vendored third-party libraries

Luban statically links these single-header libraries. Their license texts ship in `third_party/`:

| Library | Path | License |
|---|---|---|
| nlohmann/json | `third_party/json.hpp` | MIT (see [their LICENSE.MIT](https://github.com/nlohmann/json/blob/develop/LICENSE.MIT)) |
| richgel999/miniz | `third_party/miniz.{h,c}` | BSD-3-Clause (`third_party/miniz.LICENSE`) |
| marzer/tomlplusplus | `third_party/toml.hpp` | MIT (`third_party/toml.LICENSE`) |
| jothepro/doxygen-awesome-css | `docs/doxygen-awesome*.{css,js}` | MIT (`docs/doxygen-awesome.LICENSE`) — used only for the API docs site, not linked into `luban.exe` |

## Tools downloaded by `luban setup`

`luban setup` downloads + installs these into `<data>/toolchains/`. Each is the upstream's own binary, with its own license:

| Tool | License | Source |
|---|---|---|
| LLVM-MinGW | Apache-2.0 + LLVM Exception (LLVM project) | github.com/mstorsjo/llvm-mingw |
| CMake | BSD-3-Clause-like (Kitware) | github.com/Kitware/CMake |
| Ninja | Apache-2.0 | github.com/ninja-build/ninja |
| MinGit (Git for Windows minimal) | GPLv2 + various deps | github.com/git-for-windows/git |
| vcpkg | MIT (Microsoft) | github.com/microsoft/vcpkg |

These remain entirely separate processes invoked by luban; their licenses do not propagate to luban itself.
