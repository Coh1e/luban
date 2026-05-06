#pragma once
// Canonical tool name lists shared by `luban doctor` and `luban describe --host`
// (perception). Two layers:
//
//   kCoreTools — must-be-on-PATH for a working luban-managed dev env. Doctor
//                emits ✗ when any of these is missing; --strict treats it as
//                a CI-gate failure. Add an entry only if its absence breaks
//                a daily-driver flow (build / debug / lint).
//
//   kEcosystemExtras — informational. Useful neighbours (Node, Python, uv) +
//                      luban-specific tools (doxygen for `luban doc`, doctest
//                      for unit tests). Probed by perception::snapshot() but
//                      not enforced; missing one is never a failure.
//
// Perception reports the union (core + extras). Doctor only reports core.
//
// Why a shared header: previously doctor.cpp and perception.cpp each owned a
// constexpr array with the first 9 entries duplicated verbatim. Adding a tool
// required touching both, which we'd already drifted on once.

#include <array>
#include <string_view>

namespace luban::tool_list {

inline constexpr std::array<std::string_view, 9> kCoreTools = {
    "clang", "clang++", "clangd", "clang-format", "clang-tidy",
    "cmake", "ninja", "git", "vcpkg",
};

inline constexpr std::array<std::string_view, 5> kEcosystemExtras = {
    "node", "python", "uv", "doxygen", "doctest",
};

}  // namespace luban::tool_list
