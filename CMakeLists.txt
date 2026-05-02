cmake_minimum_required(VERSION 3.25)
project(luban
    VERSION 0.1.2
    LANGUAGES CXX C)

# Single source of truth for the version. cli.cpp / self.cpp / download.cpp
# all read from the generated header; release flow is "edit VERSION above,
# rebuild" — no more `sed kVersion` dance across multiple files.
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/version.hpp.in"
    "${CMAKE_BINARY_DIR}/gen/luban/version.hpp"
    @ONLY)

# Manual-grade long_help subsystem. The mdBook chapter for `specs` is
# also embedded as a C++ raw string literal so `luban specs --help`
# (and `specs/HOW-TO-USE.md` in scaffolded projects) matches the docs
# byte-for-byte. Bumping the .md, the binary's --help follows on rebuild.
#
# Chosen verbs: specs (manual-grade by design); new and setup are also
# entry-point decisions where users benefit from a thorough --help.
set(EMBED_HELP_DIR "${CMAKE_BINARY_DIR}/gen/luban/embedded_help")
# specs / new / setup get manual-grade --help. The C++ symbol always carries
# a `_help` suffix to (a) avoid colliding with C++ keywords (`new`) and
# (b) read clearly at use sites (`embedded_help::new_help`).
foreach(VERB IN ITEMS specs new setup)
    add_custom_command(
        OUTPUT  "${EMBED_HELP_DIR}/${VERB}.hpp"
        COMMAND "${CMAKE_COMMAND}"
                -DINPUT=${CMAKE_SOURCE_DIR}/docs/src/commands/${VERB}.md
                -DOUTPUT=${EMBED_HELP_DIR}/${VERB}.hpp
                -DSYMBOL=${VERB}_help
                -P ${CMAKE_SOURCE_DIR}/cmake/embed_text.cmake
        DEPENDS ${CMAKE_SOURCE_DIR}/docs/src/commands/${VERB}.md
                ${CMAKE_SOURCE_DIR}/cmake/embed_text.cmake
        VERBATIM)
    list(APPEND LUBAN_EMBED_HELP_HEADERS "${EMBED_HELP_DIR}/${VERB}.hpp")
endforeach()

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
    src/msvc_env.cpp
    src/marker_block.cpp
    src/file_util.cpp
    src/path_search.cpp
    third_party/miniz.c
    src/commands/doctor.cpp
    src/commands/env.cpp
    src/commands/new_project.cpp
    src/commands/build_project.cpp
    src/commands/shim_cmd.cpp
    src/commands/setup.cpp
    src/commands/add.cpp
    src/commands/target.cpp
    src/commands/which_search.cpp
    src/commands/describe.cpp
    src/commands/self.cpp
    src/commands/completion.cpp
    src/commands/doc.cpp
    src/commands/specs.cpp
    ${LUBAN_EMBED_HELP_HEADERS}    # ensure custom_command runs before the TU
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
    # Static link on Windows so luban.exe runs on a fresh Win10 box with no PATH
    # (no DLL dependency on libc++/libunwind/libwinpthread). On POSIX this is
    # famously hard (glibc) and unnecessary — packagers expect dynamic libc.
    if(NOT MSVC)
        target_link_options(luban PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
else()
    # POSIX:
    # - download.cpp uses std::thread (chunked Range downloads) → pthread.
    # - hash.cpp uses OpenSSL EVP for sha256/sha512/sha1/md5 (ADR-0006
    #   Phase B). System libssl is the pragmatic choice — Ubuntu/Debian/
    #   Fedora/Homebrew all ship it.
    # - download.cpp uses libcurl for HTTP on POSIX (ADR-0006 Phase B
    #   half 2). Same system-lib reasoning as OpenSSL — every distro
    #   ships it, and libcurl is the de-facto C HTTP client.
    find_package(Threads REQUIRED)
    find_package(OpenSSL REQUIRED)
    find_package(CURL REQUIRED)
    target_link_libraries(luban PRIVATE Threads::Threads OpenSSL::Crypto CURL::libcurl)
endif()

# ---- luban-shim.exe : tiny native exe proxy (rustup-style) ----
# Hard-linked into <data>/bin/<alias>.exe for every alias. Reads its own
# filename + .shim-table.json sibling to discover target exe, then
# CreateProcessW. Static-linked, ~200 KB.
#
# Windows-only: the shim model exists because PATH alias resolution on
# Windows requires a real .exe (search picks the first .exe matching name,
# .cmd shims are ignored by tools that use CreateProcess directly). On
# POSIX, alias resolution via PATH already works for plain symlinks, so
# luban-shim.exe is unneeded — see docs/specs/decisions/ADR-0006-posix-port.md.
if(WIN32)
    add_executable(luban-shim src/shim_exe/main.cpp)
    target_include_directories(luban-shim PRIVATE ${CMAKE_SOURCE_DIR}/third_party)

    if(NOT MSVC)
        target_compile_options(luban-shim PRIVATE -Wall -Wextra)
        target_link_options(luban-shim PRIVATE -static -static-libgcc -static-libstdc++)
    endif()

    target_compile_definitions(luban-shim PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(luban-shim PRIVATE shell32)
endif()

# ---- luban-tests : doctest unit tests (ADR-0004) ----
# EXCLUDE_FROM_ALL so default `cmake --build` doesn't compile tests; opt in
# via `cmake --build --target luban-tests`. Per-module test files live under
# tests/ and pull in only the leaf modules they actually exercise (log,
# lib_targets, hash, luban_toml, paths) — keeps the test binary small and
# the link surface tight.
option(LUBAN_BUILD_TESTS "Define luban-tests target (excluded from ALL)" ON)
if(LUBAN_BUILD_TESTS)
    add_executable(luban-tests EXCLUDE_FROM_ALL
        tests/test_main.cpp
        tests/test_hash.cpp
        tests/test_lib_targets.cpp
        tests/test_luban_toml.cpp
        tests/test_marker_block.cpp
        tests/test_file_util.cpp
        tests/test_path_search.cpp
        tests/test_paths.cpp
        tests/test_selection.cpp
        tests/test_vcpkg_manifest.cpp
        tests/test_scoop_manifest.cpp
        tests/test_archive.cpp
        # Leaf production sources we exercise. NOT main.cpp / cli.cpp /
        # commands/ — those are integration territory (smoke.bat covers
        # them end-to-end).
        src/log.cpp
        src/lib_targets.cpp
        src/hash.cpp
        src/luban_toml.cpp
        src/paths.cpp
        src/marker_block.cpp
        src/file_util.cpp
        src/path_search.cpp
        src/selection.cpp
        src/vcpkg_manifest.cpp
        src/scoop_manifest.cpp
        src/archive.cpp
        third_party/miniz.c
    )
    target_include_directories(luban-tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party
        ${CMAKE_BINARY_DIR}/gen
    )
    if(MSVC)
        target_compile_options(luban-tests PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(luban-tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()
    if(WIN32)
        target_link_libraries(luban-tests PRIVATE shell32 ole32 advapi32 bcrypt)
        target_compile_definitions(luban-tests PRIVATE
            UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
        if(NOT MSVC)
            # Static-link on Windows for portability across runners
            # (CI image, dev shells via git-bash) without DLL discovery.
            target_link_options(luban-tests PRIVATE -static -static-libgcc -static-libstdc++)
        endif()
    else()
        # POSIX: same Threads + OpenSSL deps as the luban target. luban-tests
        # links src/hash.cpp which now uses EVP_* on POSIX, so it needs the
        # same crypto lib at link time.
        find_package(Threads REQUIRED)
        find_package(OpenSSL REQUIRED)
        target_link_libraries(luban-tests PRIVATE Threads::Threads OpenSSL::Crypto)
    endif()
endif()
