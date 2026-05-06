cmake_minimum_required(VERSION 3.25)
project(luban
    VERSION 0.1.1
    LANGUAGES CXX C)

# MSVC: link the C runtime statically (/MT, /MTd) so the resulting exe
# has no msvcp140.dll / vcruntime140.dll dependency. Matches the MinGW
# branch's `-static -static-libgcc -static-libstdc++` policy and keeps
# invariant 7 ("luban.exe must be static-linked — fresh Win10 + no PATH
# is enough to run") true for both compilers. CI ships MSVC builds via
# windows-latest, so this matters for release artifacts.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Single source of truth for the version. cli.cpp / self.cpp / download.cpp
# all read from the generated header; release flow is "edit VERSION above,
# rebuild" — no more `sed kVersion` dance across multiple files.
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/version.hpp.in"
    "${CMAKE_BINARY_DIR}/gen/luban/version.hpp"
    @ONLY)

# Manual-grade long_help subsystem. Per-verb markdown under templates/help/
# is embedded as a C++ raw string literal so `luban <verb> --help` matches
# that source byte-for-byte. Bumping the .md, the binary's --help follows
# on rebuild.
#
# Currently only `new` qualifies — it's the entry-point verb where users
# benefit from a thorough --help. Other verbs use inline c.long_help in
# their commands/*.cpp.
set(EMBED_HELP_DIR "${CMAKE_BINARY_DIR}/gen/luban/embedded_help")
# The C++ symbol carries a `_help` suffix to avoid colliding with C++
# keywords (`new`) and to read clearly at use sites (`embedded_help::new_help`).
foreach(VERB IN ITEMS new)
    add_custom_command(
        OUTPUT  "${EMBED_HELP_DIR}/${VERB}.hpp"
        COMMAND "${CMAKE_COMMAND}"
                -DINPUT=${CMAKE_SOURCE_DIR}/templates/help/${VERB}.md
                -DOUTPUT=${EMBED_HELP_DIR}/${VERB}.hpp
                -DSYMBOL=${VERB}_help
                -P ${CMAKE_SOURCE_DIR}/cmake/embed_text.cmake
        DEPENDS ${CMAKE_SOURCE_DIR}/templates/help/${VERB}.md
                ${CMAKE_SOURCE_DIR}/cmake/embed_text.cmake
        VERBATIM)
    list(APPEND LUBAN_EMBED_HELP_HEADERS "${EMBED_HELP_DIR}/${VERB}.hpp")
endforeach()

# Per-tool program renderers. templates/programs/<X>.lua is shipped as
# embedded_programs::<X>_lua so program_renderer.cpp can fall back to a
# built-in when the user has no override at <config>/luban/programs/<X>.lua.
# Adding a new builtin tool: drop a new templates/programs/<X>.lua, append
# X here, and #include the generated header in src/program_renderer.cpp.
set(EMBED_PROGRAMS_DIR "${CMAKE_BINARY_DIR}/gen/luban/embedded_programs")
foreach(PROG IN ITEMS git bat fastfetch yazi delta)
    add_custom_command(
        OUTPUT  "${EMBED_PROGRAMS_DIR}/${PROG}.hpp"
        COMMAND "${CMAKE_COMMAND}"
                -DINPUT=${CMAKE_SOURCE_DIR}/templates/programs/${PROG}.lua
                -DOUTPUT=${EMBED_PROGRAMS_DIR}/${PROG}.hpp
                -DSYMBOL=${PROG}_lua
                -DNAMESPACE=luban::embedded_programs
                -P ${CMAKE_SOURCE_DIR}/cmake/embed_text.cmake
        DEPENDS ${CMAKE_SOURCE_DIR}/templates/programs/${PROG}.lua
                ${CMAKE_SOURCE_DIR}/cmake/embed_text.cmake
        VERBATIM)
    list(APPEND LUBAN_EMBED_PROGRAMS_HEADERS "${EMBED_PROGRAMS_DIR}/${PROG}.hpp")
endforeach()

# DESIGN §9.10 议题 AG terminal vision (2026-05-06): the luban binary
# embeds zero blueprints. All bps come from registered bp sources via
# `luban bp src add <url>`. The foundation 3 (git-base / cpp-base /
# cli-quality) live in https://github.com/Coh1e/luban-bps and are
# fetched on demand.
#
# (Earlier revisions of luban embedded `templates/blueprints/*.toml`
# into the binary as a built-in `main` source. That mechanism, plus the
# auto-generated lookup table at gen/luban/main_source_table.hpp, has
# been removed. See git history if you need the old wiring.)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Lua 5.4 (vendored at third_party/lua54/). Compiled as part of luban target;
# we embed Lua, not link the standalone REPL or compiler binary. lua.c and
# luac.c (CLI entrypoints) were intentionally excluded when vendoring.
file(GLOB LUA54_SOURCES "${CMAKE_SOURCE_DIR}/third_party/lua54/*.c")

# QuickJS-NG (vendored at third_party/quickjs/). Same policy as Lua: we
# embed the engine, not the standalone CLI; qjs.c / qjsc.c / quickjs-libc.c
# were excluded when vendoring (luban implements its own sandbox-friendly
# host API in src/qjs_engine.cpp). JS is a second-class scripting layer
# in luban — see docs/DESIGN.md §10.
file(GLOB QJS_SOURCES "${CMAKE_SOURCE_DIR}/third_party/quickjs/*.c")

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
    src/hash.cpp
    src/download.cpp
    src/archive.cpp
    src/luban_toml.cpp
    src/vcpkg_manifest.cpp
    src/lib_targets.cpp
    src/luban_cmake_gen.cpp
    src/perception.cpp
    src/msvc_env.cpp
    src/file_util.cpp
    src/path_search.cpp
    third_party/miniz.c
    ${LUA54_SOURCES}
    ${QJS_SOURCES}
    src/lua_engine.cpp
    src/lua_engine_download.cpp
    src/qjs_engine.cpp
    src/blueprint_toml.cpp
    src/blueprint_lua.cpp
    src/blueprint_lock.cpp
    src/source_resolver.cpp
    src/source_resolver_github.cpp
    src/source_registry.cpp
    src/store.cpp
    src/store_fetch.cpp
    src/file_deploy.cpp
    src/external_skip.cpp
    src/lua_json.cpp
    src/program_renderer.cpp
    src/platform.cpp
    src/generation.cpp
    src/xdg_shim.cpp
    src/blueprint_apply.cpp
    src/blueprint_reconcile.cpp
    src/commands/blueprint.cpp
    src/commands/bp_source.cpp
    src/commands/migrate.cpp
    ${LUBAN_EMBED_PROGRAMS_HEADERS}    # ensure embed cmd runs before TU
    src/commands/doctor.cpp
    src/commands/env.cpp
    src/commands/new_project.cpp
    src/commands/build_project.cpp
    src/commands/shim_cmd.cpp
    src/commands/add.cpp
    src/commands/target.cpp
    src/commands/which_search.cpp
    src/commands/describe.cpp
    src/commands/self.cpp
    src/commands/completion.cpp
    src/commands/doc.cpp
    src/commands/check.cpp
    src/commands/test_cmd.cpp
    src/commands/clean.cpp
    src/commands/fmt.cpp
    src/commands/tree.cpp
    src/commands/update.cpp
    src/commands/outdated.cpp
    ${LUBAN_EMBED_HELP_HEADERS}    # ensure custom_command runs before the TU
)

add_executable(luban ${LUBAN_SOURCES})

target_include_directories(luban PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/third_party/lua54
    ${CMAKE_SOURCE_DIR}/third_party/quickjs
    ${CMAKE_BINARY_DIR}/gen
)

if(MSVC)
    target_compile_options(luban PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(luban PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Suppress warnings on vendored upstream C sources (Lua + QuickJS). They're
# auditable but we don't keep up with their warning-pedantic level. Apply
# per-file rather than globally so our own code stays loud.
if(MSVC)
    set_source_files_properties(${LUA54_SOURCES} ${QJS_SOURCES}
        PROPERTIES COMPILE_FLAGS "/w")
else()
    set_source_files_properties(${LUA54_SOURCES} ${QJS_SOURCES}
        PROPERTIES COMPILE_FLAGS "-w")
endif()

# QuickJS uses computed gotos (gcc/clang) on POSIX for fast bytecode dispatch
# but falls back to switch on MSVC. The vendored quickjs.h handles this via
# QJS_CONFIG_USE_COMPUTED_GOTOS auto-detection — no extra defines needed here.

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
    # Lua's loadlib.c uses dlopen/dlclose on POSIX (libdl).
    # Lua's lmathlib.c uses libm (math).
    target_link_libraries(luban PRIVATE ${CMAKE_DL_LIBS} m)
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
# luban-shim.exe is unneeded — see docs/DESIGN.md §23 (POSIX port phases).
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
        tests/test_file_util.cpp
        tests/test_path_search.cpp
        tests/test_paths.cpp
        tests/test_vcpkg_manifest.cpp
        tests/test_archive.cpp
        tests/test_lua_engine.cpp
        tests/test_qjs_engine.cpp
        tests/test_blueprint_toml.cpp
        tests/test_blueprint_lua.cpp
        tests/test_blueprint_lock.cpp
        tests/test_source_resolver.cpp
        tests/test_store.cpp
        tests/test_file_deploy.cpp
        tests/test_external_skip.cpp
        tests/test_lua_json.cpp
        tests/test_program_renderer.cpp
        tests/test_generation.cpp
        tests/test_blueprint_reconcile.cpp
        # Leaf production sources we exercise. NOT main.cpp / cli.cpp /
        # commands/ — those are integration territory (smoke.bat covers
        # them end-to-end).
        src/log.cpp
        src/lib_targets.cpp
        src/hash.cpp
        src/luban_toml.cpp
        src/paths.cpp
        src/file_util.cpp
        src/path_search.cpp
        src/vcpkg_manifest.cpp
        src/archive.cpp
        src/lua_engine.cpp
        src/qjs_engine.cpp
        src/blueprint_toml.cpp
        src/blueprint_lua.cpp
        src/blueprint_lock.cpp
        src/source_resolver.cpp
        # store.cpp is the identity/lookup half (no download deps). The
        # fetch() implementation lives in store_fetch.cpp which we
        # deliberately don't link here — fetch needs WinHTTP/libcurl
        # + miniz + a dozen other things, and the unit tests don't
        # exercise it (network territory; covered by smoke.bat).
        src/store.cpp
        src/file_deploy.cpp
        src/external_skip.cpp
        src/lua_json.cpp
        src/program_renderer.cpp
        src/generation.cpp
        src/xdg_shim.cpp
        src/blueprint_reconcile.cpp
        # program_renderer.cpp #include's the embedded_programs/<X>.hpp
        # files generated by the foreach above; tests need the dependency
        # so the embed step runs before the TU is compiled.
        ${LUBAN_EMBED_PROGRAMS_HEADERS}
        third_party/miniz.c
        ${LUA54_SOURCES}
        ${QJS_SOURCES}
    )
    target_include_directories(luban-tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party
        ${CMAKE_SOURCE_DIR}/third_party/lua54
        ${CMAKE_SOURCE_DIR}/third_party/quickjs
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
        # same crypto lib at link time. Lua needs libdl + libm.
        find_package(Threads REQUIRED)
        find_package(OpenSSL REQUIRED)
        target_link_libraries(luban-tests PRIVATE Threads::Threads OpenSSL::Crypto ${CMAKE_DL_LIBS} m)
    endif()
endif()
