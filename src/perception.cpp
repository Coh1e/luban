#include "perception.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#include <intrin.h>
#include <sysinfoapi.h>
#include "util/win.hpp"
#else
#include <fstream>
#include <sys/utsname.h>
#include <unistd.h>
#endif

#include "paths.hpp"

namespace luban::perception {

namespace fs = std::filesystem;

namespace {

// Tools whose presence on PATH is informative for project-level advice.
// Matches doctor.cpp's list, with extras likely to be useful in AGENTS.md
// rendering (e.g., is bun/deno installed? user might prefer over node).
constexpr std::array<const char*, 14> kProbeTools = {
    "clang", "clang++", "clangd", "clang-format", "clang-tidy",
    "cmake", "ninja", "git", "vcpkg",
    "node", "python", "uv", "doxygen", "doctest",
};

#ifdef _WIN32
// __cpuid wrapper. Returns array {EAX, EBX, ECX, EDX} for the given leaf.
// MSVC and Clang both expose this intrinsic; arg shape is identical.
struct Cpuid { int regs[4]; };
Cpuid cpuid(int leaf, int subleaf = 0) {
    Cpuid r{};
    __cpuidex(r.regs, leaf, subleaf);
    return r;
}

// Read CPU brand from leaves 0x80000002–0x80000004 (48 bytes total).
std::string cpu_brand_from_cpuid() {
    auto ext = cpuid(0x80000000);
    if (static_cast<unsigned>(ext.regs[0]) < 0x80000004u) return "";
    char buf[49] = {};
    int* dst = reinterpret_cast<int*>(buf);
    for (int leaf = 0x80000002; leaf <= 0x80000004; ++leaf) {
        auto c = cpuid(leaf);
        std::memcpy(dst, c.regs, sizeof(c.regs));
        dst += 4;
    }
    // Trim leading/trailing spaces — Intel brand strings often have leading runs.
    std::string s(buf);
    while (!s.empty() && s.front() == ' ') s.erase(0, 1);
    while (!s.empty() && (s.back() == '\0' || s.back() == ' ')) s.pop_back();
    return s;
}

// Read CPU vendor from leaf 0 (12 bytes in EBX/EDX/ECX order).
std::string cpu_vendor_from_cpuid() {
    auto c = cpuid(0);
    char buf[13] = {};
    std::memcpy(buf + 0, &c.regs[1], 4);  // EBX
    std::memcpy(buf + 4, &c.regs[3], 4);  // EDX
    std::memcpy(buf + 8, &c.regs[2], 4);  // ECX
    return std::string(buf);
}

// Detect SIMD features via standard CPUID feature bits. Conservative —
// only includes flags we know we can reach via cpuid leaf 1 / 7.
std::vector<std::string> cpu_features_from_cpuid() {
    std::vector<std::string> out;
    auto leaf1 = cpuid(1);
    auto leaf7 = cpuid(7, 0);
    int ecx1 = leaf1.regs[2], edx1 = leaf1.regs[3];
    int ebx7 = leaf7.regs[1], ecx7 = leaf7.regs[2];

    // leaf 1 EDX
    if (edx1 & (1 << 25)) out.emplace_back("sse");
    if (edx1 & (1 << 26)) out.emplace_back("sse2");
    // leaf 1 ECX
    if (ecx1 & (1 <<  0)) out.emplace_back("sse3");
    if (ecx1 & (1 <<  9)) out.emplace_back("ssse3");
    if (ecx1 & (1 << 19)) out.emplace_back("sse4_1");
    if (ecx1 & (1 << 20)) out.emplace_back("sse4_2");
    if (ecx1 & (1 << 28)) out.emplace_back("avx");
    if (ecx1 & (1 << 12)) out.emplace_back("fma");
    // leaf 7 EBX
    if (ebx7 & (1 <<  5)) out.emplace_back("avx2");
    if (ebx7 & (1 << 16)) out.emplace_back("avx512f");
    if (ebx7 & (1 << 17)) out.emplace_back("avx512dq");
    if (ebx7 & (1 << 30)) out.emplace_back("avx512bw");
    if (ebx7 & (1 << 31)) out.emplace_back("avx512vl");
    // leaf 7 ECX
    if (ecx7 & (1 << 11)) out.emplace_back("avx512vnni");
    return out;
}

// Resolve OS version via the Win32 RtlGetVersion path (GetVersionEx is
// version-shimmed on modern Windows and lies). Falls back to a placeholder.
std::string windows_version() {
    typedef LONG (NTAPI *RtlGetVersionFn)(PRTL_OSVERSIONINFOW);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return "Windows";
    auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
    if (!fn) return "Windows";
    RTL_OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (fn(&info) != 0) return "Windows";
    return std::to_string(info.dwMajorVersion) + "."
         + std::to_string(info.dwMinorVersion) + "."
         + std::to_string(info.dwBuildNumber);
}

std::string arch_string() {
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    switch (si.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64: return "x86_64";
        case PROCESSOR_ARCHITECTURE_ARM64: return "arm64";
        case PROCESSOR_ARCHITECTURE_INTEL: return "x86";
        default: return "unknown";
    }
}
#endif

// PATH lookup for tool-presence probing. Same shape as doctor.cpp's `which`
// but local to this TU so perception stays self-contained.
bool tool_on_path(std::string_view tool) {
#ifdef _WIN32
    std::wstring wname = win::from_utf8(tool);
    static const std::array<const wchar_t*, 5> exts = {L".exe", L".cmd", L".bat", L".com", L""};
    for (auto ext : exts) {
        std::wstring trial = wname;
        if (ext[0] != 0) trial += ext;
        wchar_t buf[MAX_PATH * 4] = {};
        LPWSTR fp = nullptr;
        DWORD got = SearchPathW(nullptr, trial.c_str(), nullptr,
                                static_cast<DWORD>(std::size(buf)), buf, &fp);
        if (got > 0 && got < std::size(buf)) return true;
    }
    return false;
#else
    const char* path = std::getenv("PATH");
    if (!path) return false;
    std::string p(path);
    size_t start = 0;
    while (start <= p.size()) {
        size_t end = p.find(':', start);
        if (end == std::string::npos) end = p.size();
        fs::path candidate = fs::path(p.substr(start, end - start)) / std::string(tool);
        std::error_code ec;
        if (fs::exists(candidate, ec)) return true;
        if (end == p.size()) break;
        start = end + 1;
    }
    return false;
#endif
}

std::string getenv_str(const char* name) {
    auto v = paths::from_env(name);
    return v ? v->string() : std::string();
}

}  // namespace

Host snapshot() {
    Host h;

#ifdef _WIN32
    h.os_name = "Windows";
    h.os_version = windows_version();
    h.arch = arch_string();
    h.cpu_vendor = cpu_vendor_from_cpuid();
    h.cpu_brand = cpu_brand_from_cpuid();
    h.cpu_features = cpu_features_from_cpuid();

    // Logical CPU count — NumberOfProcessors covers online HT siblings.
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    h.cpu_threads = static_cast<int>(si.dwNumberOfProcessors);

    // Physical core count. GetLogicalProcessorInformation gives us PROC_CORE
    // entries we count directly.
    DWORD len = 0;
    GetLogicalProcessorInformation(nullptr, &len);
    if (len > 0) {
        std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> info(len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
        if (GetLogicalProcessorInformation(info.data(), &len)) {
            for (auto& r : info) {
                if (r.Relationship == RelationProcessorCore) ++h.cpu_cores;
            }
        }
    }
    if (h.cpu_cores == 0) h.cpu_cores = h.cpu_threads;  // best guess

    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) {
        h.ram_total = static_cast<int64_t>(mem.ullTotalPhys);
        h.ram_available = static_cast<int64_t>(mem.ullAvailPhys);
    }
#else
    // POSIX path — placeholder. Linux/macOS support is M4+; we still want
    // perception to not crash there, just return mostly-empty.
    struct utsname u{};
    if (uname(&u) == 0) {
        h.os_name = u.sysname;
        h.os_version = u.release;
        h.arch = u.machine;
    }
    h.cpu_threads = static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
    h.cpu_cores = h.cpu_threads;
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page_size > 0) {
        h.ram_total = static_cast<int64_t>(pages) * static_cast<int64_t>(page_size);
    }
#endif

    // Tools on PATH (cheap fan-out — ~14 SearchPathW calls).
    for (auto* tool : kProbeTools) {
        if (tool_on_path(tool)) h.tools_on_path.emplace_back(tool);
    }

    // XDG / luban env. Reading via paths::from_env gives us the same trim +
    // expanduser semantics paths.cpp uses internally.
    h.xdg.data_home    = getenv_str("XDG_DATA_HOME");
    h.xdg.cache_home   = getenv_str("XDG_CACHE_HOME");
    h.xdg.state_home   = getenv_str("XDG_STATE_HOME");
    h.xdg.config_home  = getenv_str("XDG_CONFIG_HOME");
    h.xdg.bin_home     = getenv_str("XDG_BIN_HOME");
    h.xdg.luban_prefix = getenv_str("LUBAN_PREFIX");

    h.home_dir = paths::home().string();

    return h;
}

nlohmann::json to_json(const Host& h) {
    using nlohmann::json;
    json j = json::object();
    j["schema"] = 1;
    j["os_name"] = h.os_name;
    j["os_version"] = h.os_version;
    j["arch"] = h.arch;
    j["cpu_vendor"] = h.cpu_vendor;
    j["cpu_brand"] = h.cpu_brand;
    j["cpu_cores"] = h.cpu_cores;
    j["cpu_threads"] = h.cpu_threads;
    j["cpu_features"] = h.cpu_features;
    j["ram_total"] = h.ram_total;
    j["ram_available"] = h.ram_available;
    j["tools_on_path"] = h.tools_on_path;
    j["xdg"] = json::object({
        {"data_home", h.xdg.data_home},
        {"cache_home", h.xdg.cache_home},
        {"state_home", h.xdg.state_home},
        {"config_home", h.xdg.config_home},
        {"bin_home", h.xdg.bin_home},
        {"luban_prefix", h.xdg.luban_prefix},
    });
    j["home_dir"] = h.home_dir;
    return j;
}

}  // namespace luban::perception
