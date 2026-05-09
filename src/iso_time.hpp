// `iso_time` — single-purpose helper. ISO-8601 UTC timestamp at second
// precision, e.g. "2026-05-09T12:34:56Z". Used in lock files / source
// registry timestamps (added_at / commit fallback / lock resolved_at).
//
// Was inline in src/generation.cpp::now_iso8601 until the generation
// module went away in v0.5.0 — extracted here so the surviving call
// sites (commands/blueprint.cpp, commands/bp_source.cpp) don't have
// to duplicate the chrono boilerplate.

#pragma once

#include <string>

namespace luban::iso_time {

std::string now();

}  // namespace luban::iso_time
