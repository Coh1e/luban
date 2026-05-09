// `describe_state` — build the JSON object that `luban describe` /
// `luban describe --json` emit. Split out of commands/describe.cpp so
// unit tests (tests/test_describe.cpp, DESIGN §10 item 7) can reach it
// without dragging perception.cpp's host snapshot deps into the test
// binary's link surface.
//
// Schema (frozen at "schema": 1):
//   {
//     "schema": 1,
//     "luban_version": "<X.Y.Z from luban/version.hpp>",
//     "paths": { "data": ..., "cache": ..., ... },
//     "installed_components": [],   // legacy v0.x slot, always empty in v1.0
//     "project": { ... }            // present only when cwd is in a luban project
//   }

#pragma once

#include "json.hpp"

namespace luban::describe_state {

[[nodiscard]] nlohmann::json build();

}  // namespace luban::describe_state
