#pragma once
// HKCU\Environment\PATH 操作 — rustup-style auto-PATH。
// 写完广播 WM_SETTINGCHANGE，让已开窗口的 cmd / explorer 拿到新值。

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace luban::win_path {

namespace fs = std::filesystem;

// 把 `dir` 加到 HKCU\Environment\PATH 头部（rustup 风格：toolchain bin 排前）。
// 已在 PATH 里则 no-op。返回 true 表示 PATH 真的改了。
bool add_to_user_path(const fs::path& dir);

// 反向：从 HKCU PATH 移除 `dir`。返回 true 表示真的删了一项。
bool remove_from_user_path(const fs::path& dir);

// 读 HKCU PATH 当前内容（按 ';' 切分后的 entries）。诊断用。
std::vector<std::string> read_user_path();

// HKCU\Environment\<name> 标量值的读/写/删。type 为 REG_SZ 或 REG_EXPAND_SZ。
// 写完广播 WM_SETTINGCHANGE。
bool set_user_env(const std::string& name, const std::string& value, bool expand = false);
bool unset_user_env(const std::string& name);

// 读 HKCU\Environment\<name>。Missing → nullopt。Used by `self uninstall`
// to gate `unset_user_env` on ownership checks: if the user pointed
// VCPKG_ROOT / EM_CONFIG / msvc-captured vars at a non-luban directory,
// luban must NOT clear them on uninstall.
std::optional<std::string> get_user_env(const std::string& name);

}  // namespace luban::win_path
