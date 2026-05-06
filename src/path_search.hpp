#pragma once
// PATH-lookup helper. Single owner of the logic that previously lived as
// near-identical copies in doctor.cpp and perception.cpp (and a third Win32
// variant in commands/which_search.cpp).
//
// On Windows: SearchPathW with explicit PATHEXT iteration so .cmd / .bat
// shims are picked up too (SearchPathW alone skips them when a bare name
// is queried). On POSIX: split $PATH on ':' and test fs::exists.
//
// The lookup honours **the current process PATH** — callers that want to
// search a luban-augmented PATH (e.g. commands/which_search.cpp does this
// to include toolchain dirs not yet on user PATH) must SetEnvironmentVariable
// before calling and restore after. This keeps the helper stateless.

#include <filesystem>
#include <optional>
#include <string_view>

namespace luban::path_search {

namespace fs = std::filesystem;

// Resolve `tool` against $PATH (or %PATH%). Returns absolute path on hit,
// nullopt on miss. On Windows, .exe/.cmd/.bat/.com/<bare> are tried in that
// order, mirroring cmd's resolution rules.
std::optional<fs::path> on_path(std::string_view tool);

// Convenience: return path-as-string ("" on miss). Mirrors the legacy
// doctor::which() return shape so callers don't need to optional-unpack.
inline std::string on_path_str(std::string_view tool) {
    auto p = on_path(tool);
    return p ? p->string() : std::string{};
}

}  // namespace luban::path_search
