#pragma once
// Subprocess helper — CreateProcessW on Windows, fork/exec on POSIX.

#include <map>
#include <string>
#include <vector>

namespace luban::proc {

// Run cmd[0] with cmd[1..N] in cwd, with env vars merged on top of current
// process env. Inherits stdin/stdout/stderr. Returns the child's exit code,
// or -1 on spawn failure.
int run(const std::vector<std::string>& cmd,
        const std::string& cwd,
        const std::map<std::string, std::string>& env_overrides);

}  // namespace luban::proc
