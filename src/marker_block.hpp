#pragma once
// AGENTS.md "luban-managed" marker block engine (ADR-0003).
//
// AGENTS.md is the contract surface between luban and any AI agent reading
// the project root. Sections rendered from project state (luban.toml /
// vcpkg.json / installed.json) live inside marker blocks of the form:
//
//     <!-- BEGIN luban-managed: <name> -->
//     ...content...
//     <!-- END luban-managed -->
//
// `luban specs sync` re-renders only the *body* of each known section;
// content outside the markers (and content of unknown / removed sections)
// stays untouched. This module isolates the parse/replace logic so it can
// be unit-tested independently of the filesystem-touching specs subcommand.

#include <map>
#include <string>
#include <vector>

namespace luban::marker_block {

// Section names that the templates emit, in render order. Anything else in
// the file is treated as user-owned text.
const std::vector<std::string>& managed_section_order();

// Parse a freshly-rendered template; return { section-name → body-text }.
// Body excludes the BEGIN/END markers themselves and any trailing newline
// directly after BEGIN. Multiple sections with the same name: last wins
// (templates shouldn't have duplicates).
std::map<std::string, std::string> extract_template_blocks(
    const std::string& rendered);

// Take an existing AGENTS.md text and a map of new section bodies; return
// the file with every known managed section's body replaced. Sections that
// the user removed (no BEGIN marker) stay removed. Sections whose END
// marker is missing are skipped (and a warning is logged through log::warnf).
std::string sync_managed_blocks(
    const std::string& existing,
    const std::map<std::string, std::string>& tpl_blocks);

}  // namespace luban::marker_block
