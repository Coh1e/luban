cmake_minimum_required(VERSION 3.25)
project(luban CXX C)

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
    src/bucket_sync.cpp
    src/archive.cpp
    src/component.cpp
    src/luban_toml.cpp
    src/vcpkg_manifest.cpp
    src/lib_targets.cpp
    src/luban_cmake_gen.cpp
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
)

add_executable(luban ${LUBAN_SOURCES})

target_include_directories(luban PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
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
