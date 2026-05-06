#pragma once
// Tiny file-IO helpers used across multiple commands and modules. Previously
// each consumer rolled its own near-identical version:
//
//   commands/specs.cpp:::read_text           — plain ifstream → string
//   commands/new_project.cpp::read_file      — same + UTF-8 BOM strip
//   manifest_source.cpp::read_text_strip_bom — same + UTF-8 BOM strip
//
// Plus tmp+rename atomic-write logic re-implemented in vcpkg_manifest.cpp,
// registry.cpp, download.cpp, scoop_manifest.cpp, etc.
//
// All read_* helpers return an empty string on missing file / read error
// (no exception). Callers that need to distinguish "missing" from "empty
// content" should fs::exists() before calling.

#include <filesystem>
#include <string>
#include <string_view>

namespace luban::file_util {

namespace fs = std::filesystem;

// Read the whole file as binary; return contents or "" on any failure.
std::string read_text(const fs::path& path);

// Same as read_text, but strips a leading UTF-8 BOM (EF BB BF) if present.
// Use for: JSON manifests (Scoop / Notepad save with BOM), templates,
// anything we'll feed to a tolerant parser.
std::string read_text_no_bom(const fs::path& path);

// Atomic write: write to <path>.tmp then rename onto <path>. Cross-volume
// fallback to copy + remove. Creates parent dirs as needed. Returns true
// on success.
bool write_text_atomic(const fs::path& path, std::string_view content);

}  // namespace luban::file_util
