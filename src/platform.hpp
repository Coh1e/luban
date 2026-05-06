// `platform` — tiny single-purpose host detection helper used by the
// blueprint pipeline to match a host against the `target` field on
// LockedTool platforms.
//
// We deliberately keep this separate from perception.cpp (which is the
// "describe everything about the host" subsystem with its own JSON
// schema). Here we just need three small strings — anything richer
// belongs in perception.

#pragma once

#include <string>
#include <string_view>

namespace luban::platform {

/// "windows" / "linux" / "macos" / "unknown". Matches what
/// luban.platform.os() returns to Lua blueprints.
[[nodiscard]] std::string_view host_os();

/// "x64" / "arm64" / "x86" / "unknown".
[[nodiscard]] std::string_view host_arch();

/// Concatenation: e.g. "windows-x64", "linux-arm64". The string used
/// to key into LockedTool.platforms when picking a download.
[[nodiscard]] std::string host_triplet();

}  // namespace luban::platform
