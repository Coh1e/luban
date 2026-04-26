# The Daily Driver Loop

Once you've done the one-time setup (`luban setup` + `luban env --user`), here's what a typical week with luban looks like.

## Monday: new project

```bat
luban new app weekly-experiment
cd weekly-experiment
nvim src/weekly-experiment/main.cpp     # clangd autocomplete works immediately
```

`luban new` already ran `luban build` once at the end, so `compile_commands.json` is on disk and clangd will find it without any LSP config in your editor.

## Tuesday: needed a JSON parser

```bat
luban add nlohmann-json
luban build                              # vcpkg fetches + caches it (~30s first time)
```

`luban add` edited `vcpkg.json` and regenerated `luban.cmake`. You did not open `CMakeLists.txt`. You did not look up the cmake target name (it's `nlohmann_json::nlohmann_json`, but luban's curated mapping handles that).

## Wednesday: split out a library

The `main.cpp` is getting unwieldy. Move parsing logic into its own static lib:

```bat
luban target add lib parser
```

Now you have `src/parser/{parser.h, parser.cpp, CMakeLists.txt}`. Edit those three files. Then in `src/weekly-experiment/CMakeLists.txt` add **one line**:

```cmake
target_link_libraries(weekly-experiment PRIVATE parser)
```

`luban build` again — both targets compile, the exe links the lib.

## Thursday: tests

```bat
luban add catch2
luban target add exe test-parser
```

Wire `test-parser` to depend on `parser` and `Catch2::Catch2WithMain`. Standard cmake from there.

## Friday: clone the project on a colleague's machine

The colleague has cmake + ninja + vcpkg, but no Luban:

```bat
git clone <your-repo>
cd weekly-experiment
cmake --preset default
cmake --build --preset default
```

Works. `luban.cmake` is in git, so cmake includes it; vcpkg fetches the same deps at the same baseline (locked in `vcpkg-configuration.json`). The colleague never knew Luban existed.

## What changed every day

| Day | Files Luban touched | Files you touched |
|---|---|---|
| Mon | `vcpkg.json` `luban.cmake` (initial) | none |
| Tue | `vcpkg.json` `luban.cmake` | `main.cpp` |
| Wed | `luban.cmake` (LUBAN_TARGETS) + new `src/parser/*` | `main.cpp`, `src/weekly-experiment/CMakeLists.txt` (+1 line), `parser.{h,cpp}` |
| Thu | `vcpkg.json` `luban.cmake` + new `src/test-parser/*` | various |
| Fri | (none — just running cmake) | (none — just running cmake) |

You opened the **root** `CMakeLists.txt` zero times. You wrote zero `find_package` calls. You read the vcpkg manifest mode docs zero times.

## When to escape Luban

Luban is best at the 80% case. When you need anything below, just write it directly:

- **Custom compile flags per file**: write them in `src/<target>/CMakeLists.txt`, after `luban_apply()`.
- **Custom find rules** for a non-vcpkg dep: standard `find_package(MyLib)` after `include(luban.cmake)` — `luban.cmake` doesn't fight you.
- **Unusual generators (Make, VS, Xcode)**: edit `CMakePresets.json` (it's user-owned).
- **Header-only library target**: in `src/<lib>/CMakeLists.txt`, change `add_library(<name> STATIC ...)` to `add_library(<name> INTERFACE)` and adjust accordingly.
- **External dep from somewhere not vcpkg**: skip `luban add`, write your own `find_package` / FetchContent.

The escape hatch is: just delete the `include(luban.cmake)` line from `CMakeLists.txt` and you have a plain cmake project again.
