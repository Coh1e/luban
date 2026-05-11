cmake_minimum_required(VERSION 3.25)
project(luban
    VERSION 1.0.7
    LANGUAGES CXX C)

# Windows-only target (DESIGN §1; v1.0.5 dropped POSIX support entirely).
# Refuse to configure on non-Windows hosts — keeps the error early and
# loud rather than letting a partial build limp into a confusing link
# failure.
if(NOT WIN32)
    message(FATAL_ERROR
        "luban targets Windows only as of v1.0.5 (DESIGN §1 Windows-first). "
        "POSIX builds were removed; macOS / Linux users have brew / apt / nix "
        "with first-class C++ tooling already.")
endif()

# MSVC: link the C runtime statically (/MT, /MTd) so the resulting exe
# has no msvcp140.dll / vcruntime140.dll dependency. Keeps invariant 7
# ("luban.exe must be static-linked — fresh Win10 + no PATH is enough
# to run") true on the canonical CI flavor. clang/MinGW dev builds use
# `-static` link_options below for the same effect.
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

# Per-tool config renderers. templates/configs/<X>.lua is shipped as
# embedded_configs::<X>_lua so config_renderer.cpp can fall back to a
# built-in when the user has no override at <config>/luban/configs/<X>.lua.
# Adding a new builtin tool: drop a new templates/configs/<X>.lua, append
# X here, and #include the generated header in src/config_renderer.cpp.
set(EMBED_CONFIGS_DIR "${CMAKE_BINARY_DIR}/gen/luban/embedded_configs")
foreach(CFG IN ITEMS git bat fastfetch yazi delta)
    add_custom_command(
        OUTPUT  "${EMBED_CONFIGS_DIR}/${CFG}.hpp"
        COMMAND "${CMAKE_COMMAND}"
                -DINPUT=${CMAKE_SOURCE_DIR}/templates/configs/${CFG}.lua
                -DOUTPUT=${EMBED_CONFIGS_DIR}/${CFG}.hpp
                -DSYMBOL=${CFG}_lua
                -DNAMESPACE=luban::embedded_configs
                -P ${CMAKE_SOURCE_DIR}/cmake/embed_text.cmake
        DEPENDS ${CMAKE_SOURCE_DIR}/templates/configs/${CFG}.lua
                ${CMAKE_SOURCE_DIR}/cmake/embed_text.cmake
        VERBATIM)
    list(APPEND LUBAN_EMBED_CONFIGS_HEADERS "${EMBED_CONFIGS_DIR}/${CFG}.hpp")
endforeach()

# DESIGN §9.10 议题 AG terminal vision (2026-05-06): the luban binary
# embeds zero blueprints. All bps come from registered bp sources via
# `luban bp src add <url>`. The foundation 3 (foundation / cpp-toolchain /
# cli-tools) live in https://github.com/Coh1e/luban-bps and are
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

# ---- libcurl + nghttp2 (fetched at configure time) -----------------------
# v1.0.7 swapped the curl.exe subprocess backend for in-process libcurl
# (DESIGN §1 amendment 2026-05-12). Rationale: lock the curl version
# (System32 curl varies by Windows build), force HTTP/2 negotiation
# (VN/CN network notes — HTTP/2 is the protocol that wins under throttle),
# get real-time CURLOPT_XFERINFOFUNCTION progress instead of polling
# `dest` size, drop subprocess spawn cost. SSL backend: Schannel only —
# uses Windows-native crypt32/secur32, zero added vendored crypto, keeps
# invariant 7 (static link, no DLL deps) satisfied.
#
# zlib is intentionally NOT pulled in: GitHub release URLs serve already-
# compressed archives (.zip/.tar.xz/.tar.gz) without Content-Encoding gzip,
# so libcurl's decompression layer would never engage. Skipping it saves
# the FetchContent surface + ~200 KiB of binary.
include(FetchContent)

# nghttp2: HTTP/2 framing layer. libcurl 8.x doesn't bundle an HTTP/2
# implementation; without nghttp2 it negotiates h2 → falls back to h1.1.
# ENABLE_LIB_ONLY drops app/HPACK tools/examples. BUILD_STATIC_LIBS=ON +
# BUILD_SHARED_LIBS=OFF produce only the `nghttp2_static` archive — must be
# set BEFORE FetchContent_MakeAvailable, otherwise nghttp2 picks defaults
# (shared, no static) and the link of luban fails with -lnghttp2_static.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(ENABLE_LIB_ONLY ON CACHE BOOL "" FORCE)
set(ENABLE_DOC OFF CACHE BOOL "" FORCE)
set(ENABLE_FAILMALLOC OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
# Use the release tarball, not GIT_REPOSITORY: nghttp2's git tree pulls
# in munit / mruby / neverbleed submodules (~50 MB) that we don't need,
# and CMake's FetchContent + GIT_SHALLOW + tag has a known "HEAD0" bug
# in some cmake versions when re-checking out tagged refs.
FetchContent_Declare(nghttp2
    URL https://github.com/nghttp2/nghttp2/releases/download/v1.64.0/nghttp2-1.64.0.tar.xz
    URL_HASH SHA256=88bb94c9e4fd1c499967f83dece36a78122af7d5fb40da2019c56b9ccc6eb9dd
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(nghttp2)

# Curl's FindNGHTTP2.cmake runs find_path/find_library against system paths
# and won't see the FetchContent-built nghttp2 in build/_deps. Pre-seed
# the input variables so find_package short-circuits to our build:
#  * INCLUDE_DIR points at source `lib/includes/` (nghttp2.h) AND binary
#    `lib/includes/` (generated nghttp2ver.h with the configured version).
#  * LIBRARY is the in-tree static target name; target_link_libraries
#    resolves it to the right .a/.lib at link time.
set(NGHTTP2_INCLUDE_DIR
    "${nghttp2_SOURCE_DIR}/lib/includes;${nghttp2_BINARY_DIR}/lib/includes"
    CACHE STRING "" FORCE)
set(NGHTTP2_LIBRARY nghttp2_static CACHE STRING "" FORCE)
# FindNGHTTP2.cmake reads NGHTTP2_VERSION from the include dir at find time;
# the value is informational only, but setting it suppresses a noisy probe.
set(NGHTTP2_VERSION 1.64.0 CACHE STRING "" FORCE)

# libcurl: HTTP client.
# - BUILD_SHARED_LIBS OFF + CURL_STATICLIB ON → only the static archive,
#   and consumers including <curl/curl.h> get CURL_STATICLIB defined via
#   the imported target's INTERFACE_COMPILE_DEFINITIONS.
# - HTTP_ONLY ON drops FTP/SMTP/IMAP/etc. — luban only ever does
#   HTTPS GET and HEAD; the rest is dead binary weight.
# - CURL_USE_SCHANNEL ON, all other SSL backends OFF: Windows-native TLS,
#   no OpenSSL vendor needed.
# - USE_NGHTTP2 ON wires the FetchContent-built nghttp2 above.
# - CURL_DISABLE_ALTSVC/SRP/MIME/etc. trim further; these aren't on the
#   GitHub releases path luban hits.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CURL_STATICLIB ON CACHE BOOL "" FORCE)
set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(BUILD_LIBCURL_DOCS OFF CACHE BOOL "" FORCE)
set(BUILD_MISC_DOCS OFF CACHE BOOL "" FORCE)
set(ENABLE_CURL_MANUAL OFF CACHE BOOL "" FORCE)
set(HTTP_ONLY ON CACHE BOOL "" FORCE)
set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
set(CURL_USE_MBEDTLS OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
set(USE_NGHTTP2 ON CACHE BOOL "" FORCE)
set(CURL_ZLIB OFF CACHE BOOL "" FORCE)
set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_ALTSVC ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_KERBEROS_AUTH ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_NTLM ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_SRP ON CACHE BOOL "" FORCE)
set(ENABLE_UNIX_SOCKETS OFF CACHE BOOL "" FORCE)
set(ENABLE_THREADED_RESOLVER ON CACHE BOOL "" FORCE)
# Skip the install/export target machinery — we consume libcurl as a
# build-tree dependency only. Without this, curl's CMakeLists tries to
# install libcurl_static + reference its transitive nghttp2_static, and
# CMake errors because nghttp2 is in a different export set.
set(CURL_ENABLE_EXPORT_TARGET OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(curl
    URL https://github.com/curl/curl/releases/download/curl-8_10_1/curl-8.10.1.tar.xz
    URL_HASH SHA256=73a4b0e99596a09fa5924a4fb7e4b995a85fda0d18a2c02ab9cf134bebce04ee
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(curl)

set(LUBAN_SOURCES
    src/main.cpp
    src/cli.cpp
    src/log.cpp
    src/paths.cpp
    src/env_snapshot.cpp
    src/iso_time.cpp
    src/applied_db.cpp
    src/shim.cpp
    src/proc.cpp
    src/win_path.cpp
    src/hash.cpp
    src/download.cpp
    src/libcurl_backend.cpp
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
    src/lua_engine.cpp
    src/lua_engine_download.cpp
    src/progress.cpp
    src/renderer_registry.cpp
    src/resolver_registry.cpp
    src/lua_frontend.cpp
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
    src/config_renderer.cpp
    src/platform.cpp
    src/xdg_shim.cpp
    src/blueprint_apply.cpp
    src/commands/blueprint.cpp
    src/commands/bp_source.cpp
    src/commands/msvc.cpp
    ${LUBAN_EMBED_CONFIGS_HEADERS}     # ensure embed cmd runs before TU
    src/commands/doctor.cpp
    src/commands/env.cpp
    src/commands/new_project.cpp
    src/commands/build_project.cpp
    src/commands/add.cpp
    src/commands/which_search.cpp
    src/commands/describe.cpp
    src/describe_state.cpp
    src/commands/self.cpp
    ${LUBAN_EMBED_HELP_HEADERS}    # ensure custom_command runs before the TU
)

add_executable(luban ${LUBAN_SOURCES})

target_include_directories(luban PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/third_party/lua54
    ${CMAKE_BINARY_DIR}/gen
)

if(MSVC)
    target_compile_options(luban PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(luban PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Suppress warnings on vendored upstream C sources (Lua). They're
# auditable but we don't keep up with their warning-pedantic level. Apply
# per-file rather than globally so our own code stays loud.
if(MSVC)
    set_source_files_properties(${LUA54_SOURCES} PROPERTIES COMPILE_FLAGS "/w")
else()
    set_source_files_properties(${LUA54_SOURCES} PROPERTIES COMPILE_FLAGS "-w")
endif()

# Win32 APIs used: SHGetKnownFolderPath (shell32), CoTaskMemFree (ole32),
# GetEnvironmentVariableW / CreateProcessW / GetStdHandle / SetConsoleMode (kernel32),
# RegOpenKeyExW / RegQueryValueExW / RegSetValueExW (advapi32),
# SendMessageTimeoutW (user32).
# bcrypt: hash.cpp's BCryptHash*.
# crypt32/secur32/ws2_32/wldap32/normaliz: Schannel-backend libcurl static.
#   crypt32   — certificate APIs (CertFindCertificateInStore, etc.)
#   secur32   — Schannel SSPI (InitializeSecurityContext / Schannel package)
#   ws2_32    — Winsock 2 (sockets)
#   wldap32   — LDAP transport (linked even with HTTP_ONLY because curl's
#               OS-detection in CMakeLists keeps it; no actual code path
#               from luban side reaches it)
#   normaliz  — IDN normalization (curl's punycode path; harmless to link
#               even with USE_LIBIDN2=OFF since curl still references it
#               on Win32)
target_link_libraries(luban PRIVATE
    shell32 ole32 advapi32 user32 bcrypt
    CURL::libcurl
    crypt32 secur32 ws2_32 wldap32 normaliz
)
target_compile_definitions(luban PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
# Static link so luban.exe runs on a fresh Win10 box without DLL deps
# (libc++ / libunwind / libwinpthread). MSVC achieves this via
# CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded above; clang/MinGW needs
# explicit -static linker flags here.
if(NOT MSVC)
    target_link_options(luban PRIVATE -static -static-libgcc -static-libstdc++)
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
target_compile_definitions(luban-shim PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(luban-shim PRIVATE shell32)

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
        tests/test_blueprint_lua.cpp
        tests/test_blueprint_lock.cpp
        tests/test_source_resolver.cpp
        tests/test_store.cpp
        tests/test_file_deploy.cpp
        tests/test_external_skip.cpp
        tests/test_lua_json.cpp
        tests/test_config_renderer.cpp
        tests/test_lua_register.cpp
        tests/test_blueprint_apply_dryrun.cpp
        tests/test_env_user.cpp
        tests/test_trust_model.cpp
        tests/test_self_uninstall_dryrun.cpp
        tests/test_luban_cmake_gen.cpp
        tests/test_describe.cpp
        # Stubs out luban::store::fetch so blueprint_apply.cpp links
        # without store_fetch.cpp's WinHTTP/libcurl/miniz dep tree. The
        # dry-run apply tests never reach the fetch path; an accidental
        # call returns unexpected and fails the test.
        tests/test_apply_stubs.cpp
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
        src/blueprint_lua.cpp
        src/blueprint_lock.cpp
        src/platform.cpp
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
        src/config_renderer.cpp
        src/renderer_registry.cpp
        src/resolver_registry.cpp
        src/lua_frontend.cpp
        src/xdg_shim.cpp
        # blueprint_apply needs proc (post_install) + applied_db (state).
        # Both are leaf modules; proc.cpp is just a CreateProcessW/fork
        # wrapper, applied_db.cpp is line-per-record flat-file IO.
        src/blueprint_apply.cpp
        src/applied_db.cpp
        src/proc.cpp
        # source_registry powers the trust-model tests (DESIGN §8 official
        # allowlist + read/write round-trip). Pulls in toml++ for parsing.
        src/source_registry.cpp
        # luban_cmake_gen renders the per-project luban.cmake glue file —
        # exercised in unit tests + by `luban add` / `luban remove` in smoke.
        src/luban_cmake_gen.cpp
        # describe_state is the pure JSON builder behind `luban describe`;
        # tests pin the v1 schema (DESIGN §10 item 7) without dragging
        # perception.cpp's host snapshot into the test link surface.
        src/describe_state.cpp
        # env_snapshot + win_path back env --print / env --user. msvc_env
        # is a transitive dep of env_snapshot (load() merges captured vars
        # into env_dict). All three are pure file/registry IO; no extras
        # beyond what other tests already pull in (file_util, log, paths).
        src/env_snapshot.cpp
        src/msvc_env.cpp
        src/win_path.cpp
        # config_renderer.cpp #include's the embedded_configs/<X>.hpp
        # files generated by the foreach above; tests need the dependency
        # so the embed step runs before the TU is compiled.
        ${LUBAN_EMBED_CONFIGS_HEADERS}
        third_party/miniz.c
        ${LUA54_SOURCES}
    )
    target_include_directories(luban-tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party
        ${CMAKE_SOURCE_DIR}/third_party/lua54
        ${CMAKE_BINARY_DIR}/gen
    )
    if(MSVC)
        target_compile_options(luban-tests PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(luban-tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()
    target_link_libraries(luban-tests PRIVATE shell32 ole32 advapi32 bcrypt)
    target_compile_definitions(luban-tests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    if(NOT MSVC)
        # Static-link for portability across runners (CI image, dev
        # shells via git-bash) without DLL discovery.
        target_link_options(luban-tests PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
endif()
