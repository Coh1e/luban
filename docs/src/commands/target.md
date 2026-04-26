# `luban target`

Add or remove a build target (library or executable) within the current project.

## Synopsis

```text
luban target add {lib|exe} <name>
luban target remove <name>
```

## What `target add lib mylib` creates

```
src/mylib/
├── CMakeLists.txt          # add_library(mylib STATIC mylib.cpp)
│                           # target_include_directories(mylib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
│                           # luban_apply(mylib)
├── mylib.h                 # namespace mylib { int hello(); }
└── mylib.cpp               # int mylib::hello() { return 42; }
```

`LUBAN_TARGETS` in `luban.cmake` gets appended with `mylib`. The root `CMakeLists.txt`'s `luban_register_targets()` call automatically picks it up.

## What `target add exe bench` creates

```
src/bench/
├── CMakeLists.txt          # add_executable(bench main.cpp) + luban_apply(bench)
└── main.cpp                # std::println("hello from bench!");
```

## Linking targets together

luban does NOT abstract `target_link_libraries`. To link `bench` against `mylib`, edit `src/bench/CMakeLists.txt` and add **one line**:

```cmake
target_link_libraries(bench PRIVATE mylib)
```

Then `#include "mylib.h"` in `bench/main.cpp` works — mylib's `target_include_directories(... PUBLIC)` makes its headers visible to dependents.

## `target remove`

```text
luban target remove <name>
```

Unregisters from `LUBAN_TARGETS` (so cmake stops `add_subdirectory`-ing it) but **leaves `src/<name>/` in place**. Delete that directory manually if you really want it gone.

This is deliberate: `luban target remove` is non-destructive by design.
