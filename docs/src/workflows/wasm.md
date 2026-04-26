# C++ → WebAssembly via Emscripten

Compile C++ to a `.html` + `.js` + `.wasm` bundle that runs in any modern browser, using luban-managed emscripten.

```bat
luban setup --with emscripten         :: pulls node implicitly via depends
luban new app hellowasm --target=wasm
cd hellowasm
luban build                           :: auto-detects 'wasm' preset

start build\wasm\src\hellowasm\hellowasm.html  :: open in browser
```

## What `--with emscripten` installs

- **emscripten 5.0.6** (~600 MB unpacked) — clang + lld + emcc/em++/emcmake/emmake/emar/emranlib + Binaryen (wasm-opt etc) + the JS-side toolchain
- **node.js 24.15.0 LTS** (~30 MB) — pulled in via emscripten's `depends:` field; needed to run emscripten's JS-side compilers (closure, acorn, …)

You also need **Python 3** on PATH (luban does NOT manage it). Get it from python.org or scoop. Verify with `python --version`.

After install, luban writes `<emscripten>/emscripten/.emscripten` with `LLVM_ROOT` / `BINARYEN_ROOT` / `EMSCRIPTEN_ROOT` / `NODE_JS` paths so emcc/emcmake work without `emsdk activate`.

## What `--target=wasm` scaffolds

Different from `templates/app/`:

| File | Native (`templates/app/`) | WASM (`templates/wasm-app/`) |
|---|---|---|
| `CMakePresets.json` | `default`, `no-vcpkg`, `release` | `wasm`, `wasm-debug` |
| `luban.cmake` | adds `-static -static-libgcc -static-libstdc++` | adds `-sALLOW_MEMORY_GROWTH=1 -sEXIT_RUNTIME=1` + `SUFFIX ".html"` |
| `vcpkg.json` | empty deps | empty deps (most ports don't ship wasm32 triplet yet) |

The CMakeLists / src/main.cpp / .clang-tidy / .clang-format / .vscode / .gitignore are identical to native.

## How `luban build` picks the wasm path

`luban build` auto-detects: if `CMakePresets.json` has a preset named `wasm`, it picks that and wraps the configure step with `emcmake`:

```text
cmake --preset wasm        →  emcmake cmake --preset wasm
cmake --build --preset wasm
```

`emcmake` injects `CMAKE_TOOLCHAIN_FILE=<emscripten>/cmake/Modules/Platform/Emscripten.cmake` before forwarding to cmake. After the first configure, the toolchain file is cached in `CMakeCache.txt`, so subsequent rebuilds don't need `emcmake`.

If emscripten is not installed and a wasm preset is requested, `luban build` exits with code 2 and points at `luban setup --with emscripten`.

## Output

```
build/wasm/src/hellowasm/
  hellowasm.html      generated emscripten shell page
  hellowasm.js        loader; emits Module.print etc.
  hellowasm.wasm      the actual wasm binary
```

Open `hellowasm.html` directly in a browser, or serve via:

```bat
emrun build\wasm\src\hellowasm\hellowasm.html
:: or
cd build\wasm\src\hellowasm && python -m http.server
```

## Limitations

- **vcpkg deps**: most vcpkg ports don't have a working `wasm32-emscripten` triplet. Use single-header libs (json.hpp, doctest, etc) for now — vendor under `third_party/`.
- **Threading**: requires `-pthread` + `SharedArrayBuffer` enabled headers on the server. Not on by default.
- **Filesystem**: emscripten's MEMFS is in-RAM only. For real disk, you need Node-RawFS or IDBFS (out of scope here).

## Removing emscripten

```bat
luban setup --without emscripten      :: disables in selection.json + uninstalls
luban setup --without emscripten,node :: also removes node
```

`--without` is symmetric with `--with`: disables in `selection.json` AND wipes the toolchain dir + shims. Idempotent.
