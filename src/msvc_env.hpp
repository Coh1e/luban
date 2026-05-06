#pragma once
// MSVC environment integration (Phase 1: spawn-time injection only).
//
// Goal: let `luban build` / `luban run` invoke the MSVC compiler (cl.exe,
// link.exe, msbuild.exe, ...) without users having to manually source
// `vcvarsall.bat` first. Phase 2 (write captured env to HKCU so any fresh
// shell sees cl.exe directly) is deferred — that's ~30 env vars worth of
// HKCU pollution, and Phase 1 already covers luban-spawned children.
//
// Design:
//   1. We don't install MSVC. User has Visual Studio / VS Build Tools
//      installed via Microsoft's own installer; luban detects it via
//      `vswhere.exe` (Microsoft's official discovery tool, ships with
//      every VS install at a well-known path).
//   2. `luban env --msvc-init [--arch x64]` runs vcvarsall.bat in a
//      capture-mode subshell, diffs the env before/after, and persists
//      the delta to `<state>/msvc-env.json`.
//   3. env_snapshot::env_dict() reads that file (if present) and merges
//      its entries into the env apply_to passes to spawned children. So
//      `luban build` (which goes through proc::run + apply_to) sees the
//      MSVC env automatically; users never source vcvarsall manually.
//   4. `luban env --msvc-clear` removes the captured file.

#include <filesystem>
#include <map>
#include <optional>
#include <string>

#include "json.hpp"

namespace luban::msvc_env {

namespace fs = std::filesystem;

// Captured env diff: KEY → VALUE entries that vcvarsall.bat added/changed
// versus the env it was launched in. PATH is included as a full string;
// at runtime, env_snapshot prepends MSVC's PATH addition rather than
// replacing the host PATH wholesale (see Captured::path_addition below).
struct Captured {
    // Plain env vars (everything except PATH). Examples: INCLUDE, LIB,
    // LIBPATH, VCToolsInstallDir, WindowsSdkDir, WindowsSdkVersion,
    // VCToolsRedistDir, VSCMD_ARG_HOST_ARCH, ...
    std::map<std::string, std::string> vars;

    // The leading dirs vcvarsall added to PATH (typically MSVC tool dirs +
    // Windows SDK dirs). Already separated from the host PATH suffix; safe
    // to prepend onto existing PATH at child-spawn time.
    std::string path_addition;

    // Metadata for traceability / `luban describe --json` rendering.
    std::string vs_install_path;     // e.g. "C:\Program Files\Microsoft Visual Studio\2022\BuildTools"
    std::string arch;                // "x64", "x86", "arm64", ...
    std::string captured_at;         // ISO-8601, set on save
};

// Probe for vswhere.exe at the Microsoft-documented well-known path
// (`%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe`).
// Returns empty if the file isn't present (no VS installed, or the user
// removed the installer afterwards).
fs::path find_vswhere();

// Run vswhere to find the latest VS / VS Build Tools install. Returns the
// installation root (e.g. ".../BuildTools" or ".../Community"), or empty
// if no install is found. Uses `-latest -property installationPath`.
fs::path find_install();

// Locate vcvarsall.bat under a given VS install root. Conventionally at
// `<install>\VC\Auxiliary\Build\vcvarsall.bat`. Returns empty if not found.
fs::path find_vcvarsall(const fs::path& install_path);

// Capture the env diff produced by `vcvarsall.bat <arch>`. Spawns cmd.exe
// twice (once for the baseline `set`, once for `vcvarsall && set`) and
// computes the delta. Returns nullopt if either spawn fails or vcvarsall
// reports an error; check `luban env --msvc-init --verbose` output then.
std::optional<Captured> capture(const fs::path& install_path,
                                const std::string& arch = "x64");

// Persistence under <state>/msvc-env.json. Atomic write via temp + rename.
bool save(const Captured& c);
std::optional<Captured> load();
void clear();

// Path of the persisted file (used by `describe`, `doctor`, etc.).
fs::path file_path();

}  // namespace luban::msvc_env
