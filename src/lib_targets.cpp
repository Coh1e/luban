#include "lib_targets.hpp"

#include <algorithm>
#include <array>

namespace luban::lib_targets {

namespace {

// 数据驱动表 — 每行是 (vcpkg_port, find_package_name, link_targets...)。
// 来源：vcpkg 各 port 的 usage 文件 + cmake 官方 find module 名。
// 优先收录"装机率最高"的 50 个，覆盖 hello-world / 应用开发 / 数值计算 / 系统层。
struct Entry {
    std::string_view port;
    std::string_view find_package_name;
    std::vector<std::string_view> targets;
};

const std::vector<Entry>& table() {
    static const std::vector<Entry> v = {
        // 文本 / 日志 / 序列化
        {"fmt",                "fmt",            {"fmt::fmt"}},
        {"spdlog",             "spdlog",         {"spdlog::spdlog"}},
        {"nlohmann-json",      "nlohmann_json",  {"nlohmann_json::nlohmann_json"}},
        {"yaml-cpp",           "yaml-cpp",       {"yaml-cpp"}},
        {"tinyxml2",           "tinyxml2",       {"tinyxml2::tinyxml2"}},
        {"protobuf",           "Protobuf",       {"protobuf::libprotobuf"}},
        {"flatbuffers",        "flatbuffers",    {"flatbuffers::flatbuffers"}},
        {"capnproto",          "CapnProto",      {"CapnProto::capnp"}},

        // 测试
        {"catch2",             "Catch2",         {"Catch2::Catch2WithMain"}},
        {"gtest",              "GTest",          {"GTest::gtest_main", "GTest::gmock_main"}},
        {"doctest",            "doctest",        {"doctest::doctest"}},
        {"benchmark",          "benchmark",      {"benchmark::benchmark"}},

        // STL 加强 / 范式
        {"range-v3",           "range-v3",       {"range-v3::range-v3"}},
        {"abseil",             "absl",           {"absl::strings", "absl::container_common", "absl::synchronization", "absl::time"}},
        {"tl-expected",        "tl-expected",    {"tl::expected"}},
        {"tl-optional",        "tl-optional",    {"tl::optional"}},

        // 数值 / 数学
        {"eigen3",             "Eigen3",         {"Eigen3::Eigen"}},
        {"glm",                "glm",            {"glm::glm"}},

        // Boost（按子库 add；这里收最热门 5 个）
        {"boost-system",       "Boost",          {"Boost::system"}},
        {"boost-filesystem",   "Boost",          {"Boost::filesystem"}},
        {"boost-asio",         "Boost",          {"Boost::asio"}},
        {"boost-beast",        "Boost",          {"Boost::beast"}},
        {"boost-program-options","Boost",        {"Boost::program_options"}},

        // 网络 / IO
        {"asio",               "asio",           {"asio::asio"}},
        {"curl",               "CURL",           {"CURL::libcurl"}},
        {"libssh2",            "Libssh2",        {"Libssh2::libssh2"}},
        {"openssl",            "OpenSSL",        {"OpenSSL::SSL", "OpenSSL::Crypto"}},
        {"zlib",               "ZLIB",           {"ZLIB::ZLIB"}},
        {"zstd",               "zstd",           {"zstd::libzstd_shared"}},

        // DB
        {"sqlite3",            "unofficial-sqlite3", {"unofficial::sqlite3::sqlite3"}},
        {"libpq",              "PostgreSQL",     {"PostgreSQL::PostgreSQL"}},

        // 图形 / 游戏
        {"sdl2",               "SDL2",           {"SDL2::SDL2"}},
        {"sdl3",               "SDL3",           {"SDL3::SDL3"}},
        {"glfw3",              "glfw3",          {"glfw"}},
        {"glad",               "glad",           {"glad::glad"}},
        {"imgui",              "imgui",          {"imgui::imgui"}},
        {"raylib",             "raylib",         {"raylib"}},

        // 文件 / 镜像
        {"opencv4",            "OpenCV",         {"opencv_core", "opencv_imgproc", "opencv_highgui"}},
        {"libpng",             "PNG",            {"PNG::PNG"}},
        {"libjpeg-turbo",      "JPEG",           {"JPEG::JPEG"}},
        {"stb",                "Stb",            {"Stb::Stb"}},

        // 并发 / 系统层
        {"tbb",                "TBB",            {"TBB::tbb"}},
        {"libuv",              "libuv",          {"libuv::uv"}},
        {"folly",              "folly",          {"Folly::folly"}},

        // 解析 / 文本处理
        {"re2",                "re2",            {"re2::re2"}},
        {"pcre2",              "pcre2",          {"PCRE2::8BIT"}},
        {"cli11",              "CLI11",          {"CLI11::CLI11"}},
        {"cxxopts",            "cxxopts",        {"cxxopts::cxxopts"}},

        // 加密 / 压缩
        {"bcrypt-cpp",         "bcrypt-cpp",     {"bcrypt-cpp::bcrypt-cpp"}},
        {"libarchive",         "LibArchive",     {"LibArchive::LibArchive"}},
        {"lz4",                "lz4",            {"LZ4::lz4_shared"}},

        // 基础设施
        {"date",               "date",           {"date::date"}},
        {"hedley",             "hedley",         {"hedley"}},
    };
    return v;
}

}  // namespace

std::optional<Mapping> lookup(std::string_view vcpkg_port) {
    for (auto& e : table()) {
        if (e.port == vcpkg_port) {
            Mapping m;
            m.find_package_name = std::string(e.find_package_name);
            for (auto t : e.targets) m.link_targets.emplace_back(t);
            return m;
        }
    }
    return std::nullopt;
}

}  // namespace luban::lib_targets
