// `applied_db` — flat-file persistence for what blueprint_apply created.
//
// DESIGN §11 drops generation history and rollback. The remaining state
// luban genuinely needs to remember between invocations:
//
//   1. Which bps the user has applied (so meta.requires gating can
//      verify the deps are satisfied — see DESIGN §4 capability /
//      blueprint).
//
//   2. Which shim files luban created in ~/.local/bin (so `self
//      uninstall` can sweep them without nuking sibling tools that
//      uv / pipx / claude-code put there).
//
// Both are append-only line-per-record text files under <state>/luban/.
// Idempotent re-applies are handled by dedup on read.

#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace luban::applied_db {

namespace fs = std::filesystem;

// `<state>/luban/applied.txt` — one bp name per line. Written by apply
// after a successful run; read by apply's `meta.requires` preflight.
fs::path applied_path();

// `<state>/luban/owned-shims.txt` — one absolute shim path per line.
// Written by apply after each shim install; read by `self uninstall`
// to sweep luban-owned files out of the shared XDG bin dir.
fs::path owned_shims_path();

// True iff `name` (stripped of any leading `<source>/`) appears in
// applied.txt. Missing file = empty applied set = false. Used by
// blueprint_apply for `meta.requires` gating.
bool is_applied(std::string_view name);

// Append `name` (stripped of any leading `<source>/`) to applied.txt
// if not already present. No-op when already there. Returns true on
// successful write or no-op; false on IO failure.
bool mark_applied(std::string_view name);

// Append `shim_abs_path` to owned-shims.txt. Idempotent on duplicate
// (the dedup happens at sweep time — easier than rewriting the file).
bool record_owned_shim(const fs::path& shim_abs_path);

// Read all lines of owned-shims.txt. Empty vector if missing/empty.
// Result is order-of-write; caller is responsible for dedup.
std::vector<fs::path> list_owned_shims();

// Truncate owned-shims.txt + applied.txt. Used by `self uninstall`
// after sweeping the corresponding files (so the next apply starts
// from a clean state on this machine).
void clear();

}  // namespace luban::applied_db
