# Add a vcpkg library

Need `fmt`, `spdlog`, `boost-asio`, or any of the [~2200 vcpkg ports](https://vcpkg.io/en/packages.html)?

## TL;DR

```bat
luban add fmt        # edits vcpkg.json + luban.cmake
luban build          # cmake fetches + builds fmt via vcpkg manifest mode
```

## Step-by-step

1. **In your project directory**, run `luban add <port>`. Replace `<port>` with the vcpkg port name (e.g., `fmt`, `nlohmann-json`, `boost-asio`).

2. **Use the library** in your `src/<target>/main.cpp`:

   ```cpp
   #include <fmt/core.h>
   int main() { fmt::print("hello, {}\n", "fmt"); }
   ```

3. **Build**:

   ```bat
   luban build
   ```

   The first build triggers vcpkg's manifest-mode install. It's slow on first add (vcpkg builds the lib from source) but cached afterward. Subsequent builds reuse the same `vcpkg_installed/` and just compile your code.

## Version constraints

```bat
luban add fmt              # any version (uses baseline)
luban add fmt@10           # version >= 10.0.0
luban add fmt@10.2.1       # version >= 10.2.1
```

To **pin** an exact version, edit `vcpkg.json` manually after `luban add` to add an `"overrides": [...]` entry. See [reference/vcpkg-json.md](../reference/vcpkg-json.md).

## Boost (and other multi-port libs)

Boost in vcpkg is split per-component. Add only what you need:

```bat
luban add boost-asio
luban add boost-beast
luban add boost-program-options
```

Each adds the correct `Boost::<sublib>` cmake target via Luban's curated mapping.

## When the cmake target name is wrong

Luban's table covers ~224 popular libs. For uncovered libs, Luban writes `find_package(<port>)` but **doesn't auto-link**. After first `luban build`, find the target name in `vcpkg_installed/<triplet>/share/<pkg>/usage`, then:

```cmake
# in src/<your-target>/CMakeLists.txt
target_link_libraries(<your-target> PRIVATE <ModuleName>::<target>)
```

## Removing a dep

```bat
luban remove fmt
```

`vcpkg_installed/` is **not** auto-cleaned (vcpkg manages its own cache). Delete `build/` and `vcpkg_installed/` for a clean state.

## See also

- [`luban add`](../commands/add.md) — full reference
- [vcpkg manifest mode](https://learn.microsoft.com/en-us/vcpkg/users/manifests)
- [Reference → `vcpkg.json`](../reference/vcpkg-json.md)
