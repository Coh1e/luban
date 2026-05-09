// Entry points for the `bp source` and `bp list` sub-verb families.
// The actual routing happens in commands/blueprint.cpp's run_blueprint;
// it slices the `source` / `list` token off positional and forwards
// here. Implementations live in commands/bp_source.cpp.

#pragma once

#include "../cli.hpp"

namespace luban::commands {

int run_bp_source(const cli::ParsedArgs& args);
int run_bp_list(const cli::ParsedArgs& args);

}  // namespace luban::commands
