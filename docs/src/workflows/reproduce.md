# Reproducing a luban project on another machine

A Luban project committed to git can be rebuilt anywhere by anyone — **even on machines that don't have luban installed**. This is a property of the design (see [Architecture → Design philosophy §3](../architecture/philosophy.md)), not an afterthought.

## Three scenarios

### A. Target machine has luban

```bat
git clone <repo>
cd <repo>
luban build       # vcpkg fetches deps via manifest mode at the pinned baseline
```

Same fmt version, same toolchain, same binary semantics. Done.

### B. Target machine has cmake + vcpkg, but no luban

```bat
git clone <repo>
cd <repo>
set VCPKG_ROOT=C:\path\to\vcpkg
cmake --preset default
cmake --build --preset default
```

Works. cmake includes the (committed) `luban.cmake`, vcpkg honors the (committed) `vcpkg-configuration.json` baseline. Same outputs.

This is why **`luban.cmake` is git-tracked** — it makes luban an *optional* convenience, not a build-time dependency.

### C. Bare machine

```bat
:: install luban (one binary, one URL):
curl -O https://luban.dev/luban.exe

luban setup
luban env --user
:: open fresh shell

git clone <repo>
cd <repo>
luban build
```

## What's pinned, what isn't

| Concern | Pinned by | Lockfile-equivalent |
|---|---|---|
| Toolchain (cmake, ninja, clang) | `luban.exe` version on the target machine | luban release version |
| vcpkg ports baseline | `vcpkg-configuration.json` `baseline` field (commit SHA of microsoft/vcpkg) | yes |
| Per-port version | optional `dependencies[].version>=` + `overrides[]` | yes |
| Source code | git | yes |
| Generated cmake (`luban.cmake`) | git | yes (it's just text, regenerable but committed) |

## Best practices

1. **Don't `.gitignore` `luban.cmake`** — it's the bridge that makes "no-luban" reproducibility possible.

2. **Pin a vcpkg baseline.** `luban new` does this for you (uses the latest at scaffold time). Bump it intentionally with `luban sync --baseline-now` (M3) or by hand-editing `vcpkg-configuration.json`.

3. **Document the toolchain.** A `README.md` line like *"Built with luban X.Y.Z (LLVM-MinGW 22, cmake 4.3+)"* helps future-you and contributors.

4. **Keep `vcpkg.json` minimal.** Add only what you need. Each `luban add` should correspond to a real `#include` in your code — vcpkg builds everything in the manifest, so unused deps are time tax on every clean build.

## Cross-platform reproducibility (future)

Today luban runs Windows-only. When the Linux/macOS port lands, the same vcpkg manifest will resolve different triplets (`x64-linux`, `arm64-osx` etc.). Project-level `vcpkg.json` doesn't change; only `VCPKG_TARGET_TRIPLET` differs in `CMakePresets.json`.
