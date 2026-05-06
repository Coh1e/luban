// Entry points for the `bp source` and `bp search` sub-verb families.
// The actual routing happens in commands/blueprint.cpp's run_blueprint;
// it slices the `source` / `search` token off positional and forwards
// here. Implementations live in commands/bp_source.cpp.
//
// Why we don't register top-level CLI verbs of our own: `bp source` is
// logically a deeper layer of the existing `bp` group. Adding a
// sibling top-level verb would split the doc surface (`luban bp --help`
// vs `luban bp-source --help`) for no real benefit.

#pragma once

#include "../cli.hpp"

namespace luban::commands {

int run_bp_source(const cli::ParsedArgs& args);
int run_bp_search(const cli::ParsedArgs& args);

}  // namespace luban::commands
