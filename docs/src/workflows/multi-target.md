# Multi-target project

A real C++ project usually has more than one binary: an exe + a lib it links against, or an exe + tests.

## TL;DR

```bat
luban target add lib mylib       # creates src/mylib/{.h,.cpp,CMakeLists.txt}
# Then in src/<your-exe>/CMakeLists.txt manually add:
#   target_link_libraries(<your-exe> PRIVATE mylib)
luban build
```

## Worked example

Start with a fresh project:

```bat
luban new app calc
cd calc
```

Now you have `src/calc/main.cpp`. Suppose you want to put your math logic in a separate library so you can test it.

```bat
luban target add lib mathlib
```

Result:

```
calc/
├── CMakeLists.txt              # untouched
├── luban.cmake                 # LUBAN_TARGETS now: "calc;mathlib"
└── src/
    ├── calc/
    │   ├── CMakeLists.txt
    │   └── main.cpp
    └── mathlib/                ← NEW
        ├── CMakeLists.txt
        ├── mathlib.h           ← namespace mathlib { int hello(); }
        └── mathlib.cpp
```

Edit `src/calc/CMakeLists.txt` — add **one line**:

```cmake
add_executable(calc main.cpp)
luban_apply(calc)

target_link_libraries(calc PRIVATE mathlib)   # ← add this
```

Edit `src/calc/main.cpp` to use it:

```cpp
#include <print>
#include "mathlib.h"

int main() {
    std::println("hello from calc, mathlib::hello() == {}", mathlib::hello());
    return 0;
}
```

Build + run:

```bat
luban build
build\default\src\calc\calc.exe
:: hello from calc, mathlib::hello() == 42
```

## Why one extra line?

Luban deliberately does NOT auto-link new libs to existing exes. There's no way to know which exe should depend on which lib without ambiguity (testing, plugins, etc.). cmake's `target_link_libraries` is the right place to express this — it's standard cmake, every C++ developer can read it.

What luban DOES handle:
- creating the dir + skeleton files
- registering the target in `LUBAN_TARGETS`
- making `target_include_directories(... PUBLIC)` work so `#include "mathlib.h"` resolves

## More targets

```bat
luban target add lib utils       # second lib
luban target add exe bench       # second exe (e.g., for benchmarks)
```

Each gets its own `src/<name>/` dir. Inter-link as needed via `target_link_libraries`.

## Removing

```bat
luban target remove utils       # unregisters; src/utils/ left in place
rm -rf src/utils                # if you really want it gone
```

## See also

- [`luban target`](../commands/target.md) — full reference
- [Architecture → Why no IR](../architecture/why-cmake-module.md) — why luban doesn't abstract `target_link_libraries`
