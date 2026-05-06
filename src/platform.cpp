// See `platform.hpp`.

#include "platform.hpp"

namespace luban::platform {

std::string_view host_os() {
#ifdef _WIN32
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

std::string_view host_arch() {
#if defined(_M_X64) || defined(__x86_64__)
    return "x64";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#elif defined(_M_IX86) || defined(__i386__)
    return "x86";
#else
    return "unknown";
#endif
}

std::string host_triplet() {
    std::string out;
    out.reserve(16);
    out.append(host_os());
    out.push_back('-');
    out.append(host_arch());
    return out;
}

}  // namespace luban::platform
