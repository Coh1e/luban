#pragma once
// vcpkg port name → cmake target 名（含 find_package 包名）映射。
//
// 内置 ~50 个最常用 C++ 库；miss 时调用方应让用户在 luban.toml [targets] 段手填，
// 或回退到 find_package(<port> CONFIG REQUIRED) + 留 target 给用户在 src/<x>/CMakeLists.txt 接。
//
// 字段：
// - find_package_name: cmake `find_package(<X> CONFIG REQUIRED)` 里的 X
// - link_targets: target_link_libraries(... PRIVATE Y) 的 Y 列表（Boost::asio + Boost::beast 等多 target）

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace luban::lib_targets {

struct Mapping {
    std::string find_package_name;       // find_package(X CONFIG REQUIRED)
    std::vector<std::string> link_targets;
};

// vcpkg port "fmt" → {"fmt", {"fmt::fmt"}}。
// 找不到返回 nullopt。
std::optional<Mapping> lookup(std::string_view vcpkg_port);

}  // namespace luban::lib_targets
