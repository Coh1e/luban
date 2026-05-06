// `blueprint` — the in-memory shape that parsers produce and the rest of
// the v1.0 pipeline consumes.
//
// A blueprint is a recipe; "鲁班现场造" is the act of applying it. Sources
// of a blueprint can be:
//   - <name>.toml  → parsed by blueprint_toml.cpp
//   - <name>.lua   → executed by blueprint_lua.cpp (Lua first-class)
//   - <name>.js    → executed by blueprint_qjs.cpp (QuickJS, second-class)
//
// All three paths converge on a `BlueprintSpec`. Downstream consumers
// (source_resolver, store, file_deploy, config_renderer, generation)
// only see this struct — they don't care which surface produced it.
//
// Schema philosophy: structs mirror the TOML schema 1:1 to keep parsers
// dumb. Validation happens in the parser, not at struct-construction
// time. Once a BlueprintSpec exists, it's already validated.
//
// All "tool config" (the body of `[config.X]` blocks) is held as
// nlohmann::json — that's the most flexible way to keep arbitrarily-nested
// structured data without bloating this header with another bespoke
// representation. The Lua/JS renderers consume json directly; the TOML
// parser converts toml::table → json at the boundary.
//
// v0.2.0 (议题 P, 2026-05-06): TOML keys are singular: `[tool.X]`,
// `[config.X]`, `[file."path"]`. Internal C++ struct field names mirror
// (BlueprintSpec.tools / .configs / .files). Old plural TOML keys
// (`[tools.X]` / `[programs.X]` / `[files."path"]`) are no longer
// recognised — schema breaking change.

#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "json.hpp"

namespace luban::blueprint {

/// One concrete platform variant of a tool's download. Either harvested
/// from `[[tool.X.platform]]` inline blocks (manual mode) or written
/// here by source_resolver after talking to GitHub (auto mode).
///
/// `target` is a vcpkg-style triplet stem we use to identify which
/// download applies on which host: "windows-x64", "linux-x64",
/// "macos-arm64", etc. (See DESIGN.md §11.1 for canonical names.)
struct PlatformSpec {
    std::string target;
    std::string url;
    std::string sha256;
    std::string bin;          ///< Filename of the binary inside the
                              ///< extracted archive (or "" for non-archive).
    std::string artifact_id;  ///< Filled in by store/source_resolver, not
                              ///< by user. Empty in unresolved spec.
};

/// One tool entry. `source` may be a shorthand like "github:owner/repo"
/// (for source_resolver to expand) or absent if the user provided
/// `platforms` inline.
struct ToolSpec {
    std::string name;
    std::optional<std::string> source;
    std::optional<std::string> version;
    std::vector<PlatformSpec> platforms;  ///< Empty when `source` will be
                                          ///< resolved later.

    /// Optional override for the binary name inside the extracted
    /// archive. Most tool repos publish archives where the binary
    /// shares the tool's name (`bat`, `fd`, `gh`), but a few don't
    /// (BurntSushi/ripgrep ships `rg`; sharkdp/fd-find historically
    /// had nuances). When unset, the github resolver uses `name` as
    /// the binary basename + `.exe` on Windows.
    std::optional<std::string> bin;

    /// Optional one-shot script to run immediately after extraction,
    /// before the shim is written (DESIGN §9.9). Path is interpreted
    /// relative to the extracted artifact root; must stay inside it
    /// (path-traversal guard in blueprint_apply). Canonical use case:
    /// vcpkg's `bootstrap-vcpkg.bat`. The blueprint engine spawns it
    /// via the OS shell (cmd /c on Windows, /bin/sh on POSIX) with
    /// cwd set to the artifact root; non-zero exit fails the apply.
    /// Skipped on cache hits (already-installed artifacts) since
    /// post_install is part of "install".
    std::optional<std::string> post_install;

    /// Optional override for the external_skip probe target name. When
    /// unset, blueprint_apply probes for `tool.name` directly — fine for
    /// most tools (cmake, ninja, vcpkg) where the brand and the binary
    /// share a name. Set this when they differ: e.g. an `openssh` tool
    /// brand whose canonical binary on PATH is `ssh.exe` (Windows
    /// OpenSSH at System32\OpenSSH\). Without this override probe would
    /// look for `openssh.exe`, miss the system install, and reinstall
    /// despite a working external copy.
    std::optional<std::string> external_skip;

    /// Optional list of shim aliases to write at ~/.local/bin/. Each
    /// entry is interpreted as a path RELATIVE to the extracted
    /// artifact root, mirroring `bin` field semantics — e.g.
    /// "cmd/git.exe" or "ssh.exe". The basename's stem becomes the
    /// alias name. When empty, the legacy single-shim path is used
    /// (one shim derived from `bin` or the resolved
    /// LockedPlatform.bin). Multi-shim is needed for tools that ship
    /// a family of binaries the user wants on PATH at once: openssh
    /// (ssh, ssh-keygen, ssh-agent, scp, sftp), llvm-mingw (clang,
    /// clang++, gcc, g++, ld, llvm-ar, ...).
    std::vector<std::string> shims;

    /// Optional path (relative to artifact root) to a directory; every
    /// `*.exe` (Windows) / executable file (POSIX) inside it gets a
    /// shim under `~/.local/bin/`. Resolves at apply time against the
    /// extracted store_dir, so the bp doesn't need to enumerate
    /// upstream-toolchain binary names by hand. Composes with `shims`:
    /// both lists are unioned, deduplicated by alias.
    ///
    /// Use case: llvm-mingw ships ~270 binaries in `bin/` (clang+lld+
    /// llvm-* + gcc-aliases + cross-triplet variants), and listing
    /// each one in TOML is fragile (a new toolchain release adds or
    /// removes entries; the explicit list goes stale). `shim_dir =
    /// "bin"` is the right shape for that data.
    std::optional<std::string> shim_dir;

    /// When true, apply does NOT create any PATH shim for this tool —
    /// the artifact is fetched + extracted + post_install runs, but
    /// nothing lands under `~/.local/bin/`. Use case: a "tool" that
    /// isn't a CLI binary at all (fonts, fonts files, libraries that
    /// register themselves through other channels — HKCU\Fonts +
    /// AddFontResourceEx for Maple Mono is the canonical case). The
    /// post_install hook does whatever registration is needed.
    bool no_shim = false;
};

/// One `[config.X]` block: tool name + arbitrary nested config that
/// the tool's renderer will translate to the tool's native config format.
///
/// Held as JSON because the schema is per-tool — git wants `userName` and
/// `aliases`; bat wants `theme` and `style`; etc. Renderers (Lua modules
/// in templates/configs/<name>.lua) consume this via JS_ParseJSON / Lua
/// table-from-json helpers.
///
/// `for_tool` (议题 P) lets a config block target a tool whose name
/// differs from the config's own (rare — e.g. you might want
/// `[config.zsh-theme] for_tool = "starship"` to dispatch to the
/// starship renderer with starship-specific cfg keys). When unset, the
/// renderer named by `name` is used.
struct ConfigSpec {
    std::string name;
    nlohmann::json config;
    std::optional<std::string> for_tool;  ///< Renderer/tool name override.
};

/// One `[file."<path>"]` block: a literal file the user wants deployed
/// somewhere outside luban-owned space (typically under their HOME, in
/// XDG-standard locations).
enum class FileMode {
    Replace,  ///< Overwrite target file; back up the original on first apply.
    DropIn,   ///< Write to a drop-in subdir alongside the canonical file
              ///< (e.g., ~/.gitconfig.d/<bp>.gitconfig); user's main config
              ///< file [include]s it. luban never touches the canonical file.
    Merge,    ///< JSON Merge Patch (RFC 7396) — read existing JSON file,
              ///< deep-merge the patch (`content` parsed as JSON), atomic
              ///< write back. Use case: WT settings.json themes section.
    Append,   ///< Append `content` inside a luban marker block
              ///< (`# >>> luban:<bp> >>>` ... `# <<< luban:<bp> <<<`) at
              ///< end of target file. Re-applies replace the marker
              ///< block in place, idempotent. Use case: profile.ps1
              ///< multi-bp coordination.
};

struct FileSpec {
    std::string target_path;  ///< The user-facing XDG-style path, possibly
                              ///< starting with "~/" — substitute at deploy
                              ///< time, NOT at parse time.
    std::string content;      ///< Inline TOML/Lua/JS string literal.
    FileMode mode;
};

/// `[meta]` block: dependency / mutex info between blueprints.
struct MetaSpec {
    std::vector<std::string> requires_;  ///< Other blueprint names this
                                         ///< one expects already applied.
                                         ///< (Trailing underscore avoids
                                         ///< collision with the C++26
                                         ///< `requires` keyword.)
    std::vector<std::string> conflicts;
};

/// Top-level: one blueprint file's parsed content.
struct BlueprintSpec {
    int schema = 1;            ///< Schema version of the blueprint format.
                               ///< Bumped per breaking change; reader
                               ///< stays compatible with schema=1 forever.
    std::string name;
    std::string description;
    std::vector<ToolSpec> tools;
    std::vector<ConfigSpec> configs;
    std::vector<FileSpec> files;
    MetaSpec meta;

    /// Lookup helpers — small enough to keep inline.
    [[nodiscard]] const ToolSpec* find_tool(const std::string& tool_name) const noexcept {
        for (auto& t : tools) {
            if (t.name == tool_name) return &t;
        }
        return nullptr;
    }
    [[nodiscard]] const ConfigSpec* find_config(const std::string& cfg_name) const noexcept {
        for (auto& c : configs) {
            if (c.name == cfg_name) return &c;
        }
        return nullptr;
    }
};

}  // namespace luban::blueprint
