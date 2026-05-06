// `external_skip` — answer the question "is this tool already provided
// by something other than luban (scoop / brew / pacman / system pkg) on
// this user's PATH?"
//
// When the answer is yes, blueprint::apply skips downloading our copy:
// no shim is written, no store entry is allocated, no PATH-precedence
// hijacking happens. We just record that we found an external one and
// move on — luban "leaves the user alone" (DESIGN.md §11.2 + §22).
//
// What v1 does:
// - Probe via path_search::on_path. If the tool resolves on PATH outside
//   our own ~/.local/bin/ shim space, treat as external.
// - Persist findings to <state>/external.json so doctor / status can
//   show "ripgrep is provided by scoop, version not enforced" without
//   re-probing every command.
//
// What v1 does NOT do:
// - Validate version constraints. Spawning <tool> --version and parsing
//   output is per-tool unreliable (each tool has its own format), and
//   getting it wrong is worse than not enforcing. Pushed to v1.x once
//   we have a curated parser table per tool.
// - Treat "external" as authoritative. Users can override with
//   `luban blueprint apply --force-our-copy` (also v1.x).

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace luban::external_skip {

/// One record per externally-provided tool we've seen.
struct External {
    std::string tool_name;
    std::filesystem::path resolved_path;  ///< Absolute, as found on PATH.
};

/// Probe whether `tool_name` is available on PATH from a non-luban source.
/// Returns the External record if so, nullopt otherwise. Specifically
/// excludes hits that resolve under luban's own bin dir
/// (~/.local/bin/<tool> or <data>/bin/<tool>) so our own shim never
/// counts as external.
[[nodiscard]] std::optional<External> probe(std::string_view tool_name);

/// Snapshot of all externals luban has detected so far for this user.
struct Registry {
    int schema = 1;
    std::unordered_map<std::string, External> tools;  ///< keyed by tool_name
};

/// Read <state>/external.json. Missing file → empty registry, NOT an
/// error; that's the expected first-run state.
[[nodiscard]] Registry read();

/// Atomically write <state>/external.json from the in-memory registry.
[[nodiscard]] bool write(const Registry& reg);

}  // namespace luban::external_skip
