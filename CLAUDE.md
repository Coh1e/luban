# CLAUDE.md — luban project guide

> Hints for Claude Code (or any AI agent) working in this repo. Imperative,
> recipe-driven, terse. Long-form is in `docs/src/`; don't duplicate here.

## What luban is (15-second version)

Windows-first **C++ toolchain manager + cmake/vcpkg auxiliary frontend**.
One static-linked binary (`luban.exe` ~3 MB), zero UAC, XDG-first directories.
- `luban setup` installs LLVM-MinGW + cmake + ninja + mingit + vcpkg
- `luban setup --with emscripten` opt-in to C++→WASM (pulls node via depends)
- `luban env --user` adds `<data>/bin/` to HKCU PATH (rustup-style)
- `luban new app foo` + `luban add fmt` + `luban build` → hello-world with
  vcpkg deps, with **zero** `CMakeLists.txt` editing required
- `luban new app foo --target=wasm` + `luban build` → .html/.js/.wasm output
- `luban.cmake` (a generated standard cmake module) is the only thing luban
  itself "owns" inside a project — and it's git-tracked, so the project
  builds on machines that don't have luban installed.

## Build (Windows, from a clean toolchain shell)

```bat
:: Activate luban-managed toolchain in the current shell:
call %LOCALAPPDATA%\luban\env\activate.cmd
:: …or equivalently: luban env --user once, then any new shell works.

cmake --preset default                  :: Debug, ~31 MB binary
cmake --build --preset default

cmake --preset release                  :: Release, ~3 MB binary
cmake --build --preset release

build\default\luban.exe --help          :: smoke
build\release\luban.exe --version       :: smoke
```

Unit tests (doctest, ADR-0004): `cmake --build --preset release --target luban-tests`
then `./build/release/luban-tests.exe`. ~40 cases / ~110 assertions covering
hash, lib_targets, luban_toml, and marker_block (AGENTS.md engine). Tests
are EXCLUDE_FROM_ALL — default `cmake --build` skips them.

End-to-end smoke: `scripts/smoke.bat` runs 9 steps (new → add/remove → build
→ run → specs → target add/build/remove → doctor) without vcpkg network.
Both run on every CI build.

## Repo layout (essential)

```
src/                           # luban C++23 source (~6.5k lines)
  main.cpp                     # registers every CLI verb — touch when adding one
  cli.{hpp,cpp}                # subcommand schema (flags/opts/forward_rest)
  paths.cpp                    # XDG-first 4-home resolution; respect LUBAN_PREFIX
  registry.cpp                 # installed.json schema=1 (shared with old Python)
  component.cpp                # full install pipeline; line ~188 has vcpkg
                               # post-extract bootstrap special-case
  shim.cpp                     # text shim writers (.cmd only as of v0.3)
  shim_exe/main.cpp            # luban-shim.exe — separate binary, hard-linked per alias
  commands/<verb>.cpp          # one cpp per verb
  util/win.hpp                 # utf8↔wstring conversions
src/commands/
  setup.cpp env.cpp doctor.cpp shim_cmd.cpp
  new_project.cpp build_project.cpp
  add.cpp target_cmd.cpp which_search.cpp describe.cpp
templates/app/                 # `luban new app foo` scaffolding
                               # `{{name}}` is also expanded in directory names
manifests_seed/                # default selection.json + bootstrap overlays
                               # vcpkg.json here is the special-case
                               # post-extract-bootstrap target
third_party/                   # ONLY single-header vendored libs
  json.hpp miniz.{h,c} toml.hpp + their LICENSEs
docs/                          # mdBook (src/) + Doxygen (Doxyfile)
                               # CI auto-publishes to https://luban.coh1e.com/
.github/workflows/
  build.yml                    # win build + artifact + tag→release
  docs.yml                     # mdBook + Doxygen → gh-pages
```

## Don't break these (hard architectural constraints)

1. **`luban.cmake` schema stays stable** — projects in the wild commit it.
   Changes need a v2-section + read-back compat.
2. **No new third-party deps without vendoring.** Single-header only,
   committed to `third_party/` with their LICENSE alongside.
3. **No path canonicalization.** `D:\dobby` is a junction to `C:\Users\Rust`;
   `weakly_canonical()` may resolve to the other side and break
   `relative_to()`. Use literal paths the user typed.
4. **vcpkg.json is the deps source of truth.** Don't add a `[deps]` section
   to `luban.toml` — that would create a double truth.
5. **Toolchain ≠ project libs.** `luban add cmake` must be rejected;
   toolchains live in `installed.json`, libs in `vcpkg.json`. See
   `src/commands/add.cpp` system-tools list.
6. **Static-link luban.** `target_link_options(... -static -static-libgcc
   -static-libstdc++)` — luban.exe must run on a fresh Win10 with no PATH.
7. **HKCU only** — never `HKLM` writes; never need admin.

Long-form rationale lives in `docs/src/architecture/{philosophy,two-tier-deps,
why-cmake-module}.md`.

## Common task recipes

### Add a new CLI verb `foo`

1. `src/commands/foo.cpp` — implement `int run_foo(const cli::ParsedArgs&)`,
   `void register_foo()`. Set `c.group` (`setup` / `project` / `dep` /
   `advanced`) + `c.long_help` + `c.examples`.
2. `CMakeLists.txt` — append `src/commands/foo.cpp` to `LUBAN_SOURCES`.
3. `src/main.cpp` — declare `void register_foo();` and call it.
4. `docs/src/commands/foo.md` + reference in `docs/src/SUMMARY.md` and
   `docs/src/commands/overview.md`.

### Add a vcpkg port → cmake target mapping

Edit `src/lib_targets.cpp` table. Each row: `{port, find_package_name,
{target1, target2, ...}}`. ~224 popular libs already there (rolling); PR welcome.

### Add a new bootstrappable component (e.g., a new toolchain)

1. Add `manifests_seed/<name>.json` — `version`, `url`, `hash` (sha256:hex),
   `extract_dir` if archive uses a wrapper, and `bin: [[<rel>, <alias>], ...]`.
2. Enable it in `manifests_seed/selection.json` if it should be installed
   by default.
3. If the component needs post-extract setup (special-cased in
   `src/component.cpp` keyed by name): vcpkg → bootstrap-vcpkg.bat;
   emscripten → write `<config>/emscripten/config` with
   LLVM_ROOT/BINARYEN_ROOT/EMSCRIPTEN_ROOT/NODE_JS pointing at
   registry-resolved paths (since v0.2; earlier versions wrote
   `<install>/emscripten/.emscripten`). EM_CONFIG points emcc at it.
4. If the component depends on another (e.g., emscripten → node), declare
   `"depends": ["node"]` in the manifest. `setup.cpp::expand_depends`
   does DFS post-order so deps install first.

### Cut a release vX.Y.Z

Single source of truth lives in `CMakeLists.txt`'s `project(luban VERSION X.Y.Z)`.
cli.cpp / self_cmd.cpp / download.cpp's UA all read it via the cmake-generated
`luban/version.hpp`.

```bat
:: bump version (one line, one file)
sed -i "s|VERSION [0-9.]*|VERSION X.Y.Z|" CMakeLists.txt
:: build + smoke
cmake --build --preset release
build\release\luban.exe --version

:: tag + push — CI auto-creates release with luban.exe + luban-shim.exe + SHA256SUMS
git commit -am "Bump X.Y.Z" && git push
git tag -a vX.Y.Z -m "luban X.Y.Z — ..." && git push --tags
```

### Regenerate exe shims on this machine

```bat
luban shim          :: rewrites .cmd + .exe (hard-linked) + .shim-table.json
```

## Known quirks

- **D:\dobby junction**: `C:\Users\Rust\.local\share\luban` and
  `D:\dobby\.local\share\luban` point at the same files. Don't `canonical()`.
- **gh-pages branch is force-rewritten** by `.github/workflows/docs.yml`
  (`force_orphan: true`). Don't rely on its history.
- **gh CLI workflow scope**: pushing `.github/workflows/*` requires the
  token to have the `workflow` scope. If push rejected, run
  `gh auth refresh -s workflow`.
- **GH Pages cert vs Cloudflare orange-cloud**: must be DNS-only (gray
  cloud) for Let's Encrypt to verify. Orange-cloud breaks ACME.
- **Domain is `coh1e.com`** (not cho1e.com — that was a recovered typo
  from early DNS setup; verify by `gh api repos/Coh1e/luban/pages` →
  `cname` field).
- **Tests use vendored doctest** (ADR-0004): one `tests/test_<module>.cpp` per
  leaf module under `src/`. Add new tests to the `LUBAN_BUILD_TESTS` block in
  `CMakeLists.txt`. Don't resurrect the old `*_smoke.cpp` binaries — those
  were one-off validation programs from M2.

## Where to read more

- **Users**: https://luban.coh1e.com/ (full mdBook user manual)
- **Contributors**: https://luban.coh1e.com/api/ (Doxygen) +
  `docs/src/contributing.md`
- **Architecture rationale**: `docs/src/architecture/*.md`
- **Roadmap (deferred work)**: `docs/src/architecture/roadmap.md` — Linux/macOS
  port and `luban-init.exe` are the big known-unknowns
- **History**: `git log --oneline` — every step is on a separate commit
- **Older plan files**: `C:\Users\Rust\.claude\plans\` — superseded but useful
  for "why was X decided?"

## Critical reusable functions (don't reinvent)

| Function | File | Use |
|---|---|---|
| `paths::data_dir()` | `src/paths.cpp` | XDG-resolved data home |
| `registry::resolve_alias(name)` | `src/registry.cpp` | alias → absolute exe |
| `proc::run(cmd, cwd, env_overrides)` | `src/proc.cpp` | spawn with env merge |
| `download::download(url, dest, opts)` | `src/download.cpp` | HTTPS GET + SHA |
| `archive::extract(zip, dest)` | `src/archive.cpp` | ZIP w/ traversal guard |
| `env_snapshot::apply_to(env)` | `src/env_snapshot.cpp` | PATH + LUBAN_* overlay |
| `luban_cmake_gen::regenerate_in_project(dir, targets)` | `src/luban_cmake_gen.cpp` | rewrite project's `luban.cmake` |
| `vcpkg_manifest::{add,remove,save}` | `src/vcpkg_manifest.cpp` | edit `vcpkg.json` safely |
| `cli::Subcommand::forward_rest` | `src/cli.hpp` | argv passthrough (run-style verbs) |
