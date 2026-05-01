cmake_minimum_required(VERSION 3.25)
project(luban
    VERSION 0.1.2
    LANGUAGES CXX C)

# Single source of truth for the version. cli.cpp / self_cmd.cpp / download.cpp
# all read from the generated header; release flow is "edit VERSION above,
# rebuild" — no more `sed kVersion` dance across multiple files.
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/version.hpp.in"
    "${CMAKE_BINARY_DIR}/gen/luban/version.hpp"
    @ONLY)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(LUBAN_SOURCES
    src/main.cpp
    src/cli.cpp
    src/log.cpp
    src/paths.cpp
    src/registry.cpp
    src/env_snapshot.cpp
    src/shim.cpp
    src/proc.cpp
    src/win_path.cpp
    src/scoop_manifest.cpp
    src/hash.cpp
    src/download.cpp
    src/selection.cpp
    src/manifest_source.cpp
    src/archive.cpp
    src/component.cpp
    src/luban_toml.cpp
    src/vcpkg_manifest.cpp
    src/lib_targets.cpp
    src/luban_cmake_gen.cpp
    src/perception.cpp
    third_party/miniz.c
    src/commands/doctor.cpp
    src/commands/env.cpp
    src/commands/new_project.cpp
    src/commands/build_project.cpp
    src/commands/shim_cmd.cpp
    src/commands/setup.cpp
    src/commands/add.cpp
    src/commands/target_cmd.cpp
    src/commands/which_search.cpp
    src/commands/describe.cpp
    src/commands/self_cmd.cpp
    src/commands/completion.cpp
    src/commands/doc.cpp
)

add_executable(luban ${LUBAN_SOURCES})

target_include_directories(luban PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_BINARY_DIR}/gen
)

if(MSVC)
    target_compile_options(luban PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(luban PRIVATE -Wall -Wextra -Wpedantic)
    # Static link to avoid runtime DLL dependency on libc++/libunwind/libwinpthread.
    # See plan §7: hello.exe DLL-not-found incident.
    target_link_options(luban PRIVATE -static -static-libgcc -static-libstdc++)
endif()

if(WIN32)
    # Win32 APIs used: SHGetKnownFolderPath (shell32), CoTaskMemFree (ole32),
    # GetEnvironmentVariableW / CreateProcessW / GetStdHandle / SetConsoleMode (kernel32),
    # RegOpenKeyExW / RegQueryValueExW / RegSetValueExW (advapi32),
    # SendMessageTimeoutW (user32).
    target_link_libraries(luban PRIVATE shell32 ole32 advapi32 user32 bcrypt winhttp)
    target_compile_definitions(luban PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
endif()

# ---- luban-shim.exe : tiny native exe proxy (rustup-style) ----
# Hard-linked into <data>/bin/<alias>.exe for every alias. Reads its own
# filename + .shim-table.json sibling to discover target exe, then
# CreateProcessW. Static-linked, ~200 KB.
add_executable(luban-shim src/shim_exe/main.cpp)
target_include_directories(luban-shim PRIVATE ${CMAKE_SOURCE_DIR}/third_party)

if(NOT MSVC)
    target_compile_options(luban-shim PRIVATE -Wall -Wextra)
    target_link_options(luban-shim PRIVATE -static -static-libgcc -static-libstdc++)
endif()

if(WIN32)
    target_compile_definitions(luban-shim PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(luban-shim PRIVATE shell32)
endif()
