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

        // ==== 第二批扩展（v0.1.3+）====

        // Boost 子库（除已有的 system/filesystem/asio/beast/program-options）
        {"boost-thread",          "Boost",       {"Boost::thread"}},
        {"boost-iostreams",       "Boost",       {"Boost::iostreams"}},
        {"boost-test",            "Boost",       {"Boost::unit_test_framework"}},
        {"boost-regex",           "Boost",       {"Boost::regex"}},
        {"boost-date-time",       "Boost",       {"Boost::date_time"}},
        {"boost-chrono",          "Boost",       {"Boost::chrono"}},
        {"boost-uuid",            "Boost",       {"Boost::uuid"}},
        {"boost-locale",          "Boost",       {"Boost::locale"}},
        {"boost-property-tree",   "Boost",       {"Boost::property_tree"}},
        {"boost-serialization",   "Boost",       {"Boost::serialization"}},
        {"boost-url",             "Boost",       {"Boost::url"}},
        {"boost-log",             "Boost",       {"Boost::log"}},

        // 网络 / HTTP
        {"cpp-httplib",           "httplib",                  {"httplib::httplib"}},
        {"cpr",                   "cpr",                      {"cpr::cpr"}},
        {"cppzmq",                "cppzmq",                   {"cppzmq"}},
        {"mongoose",              "unofficial-mongoose",      {"unofficial::mongoose::mongoose"}},
        {"drogon",                "Drogon",                   {"Drogon::Drogon"}},
        {"crow",                  "Crow",                     {"Crow::Crow"}},
        {"oatpp",                 "oatpp",                    {"oatpp::oatpp"}},
        {"libwebsockets",         "libwebsockets",            {"websockets"}},

        // DB
        {"hiredis",               "hiredis",                  {"hiredis::hiredis"}},
        {"libpqxx",               "libpqxx",                  {"libpqxx::pqxx"}},
        {"mysql-connector-cpp",   "unofficial-mysql-connector-cpp", {"unofficial::mysql-connector-cpp::connector"}},
        {"sqlite-orm",            "sqlite_orm",               {"sqlite_orm::sqlite_orm"}},
        {"mongo-cxx-driver",      "mongocxx",                 {"mongo::mongocxx_shared"}},

        // 日志
        {"glog",                  "glog",                     {"glog::glog"}},
        {"easyloggingpp",         "easyloggingpp",            {"easyloggingpp"}},
        {"plog",                  "plog",                     {"plog::plog"}},

        // 加密 / 安全
        {"libsodium",             "unofficial-sodium",        {"unofficial-sodium::sodium"}},
        {"mbedtls",               "MbedTLS",                  {"MbedTLS::mbedtls"}},
        {"botan",                 "Botan",                    {"Botan::Botan"}},
        {"cryptopp",              "cryptopp",                 {"cryptopp::cryptopp"}},

        // 压缩
        {"brotli",                "unofficial-brotli",        {"unofficial::brotli::brotlidec", "unofficial::brotli::brotlienc"}},
        {"snappy",                "Snappy",                   {"Snappy::snappy"}},

        // XML / parser
        {"expat",                 "expat",                    {"expat::expat"}},
        {"libxml2",               "LibXml2",                  {"LibXml2::LibXml2"}},
        {"pugixml",               "pugixml",                  {"pugixml::pugixml"}},
        {"rapidxml",              "rapidxml",                 {"rapidxml"}},

        // 音频
        {"openal-soft",           "OpenAL",                   {"OpenAL::OpenAL"}},
        {"miniaudio",             "miniaudio",                {"miniaudio::miniaudio"}},
        {"libsndfile",            "SndFile",                  {"SndFile::sndfile"}},

        // 数学 / ML
        {"armadillo",             "Armadillo",                {"armadillo"}},
        {"dlib",                  "dlib",                     {"dlib::dlib"}},
        {"flann",                 "flann",                    {"flann::flann"}},

        // JSON / 序列化（除已有 nlohmann-json）
        {"simdjson",              "simdjson",                 {"simdjson::simdjson"}},
        {"rapidjson",             "RapidJSON",                {"rapidjson"}},
        {"cereal",                "cereal",                   {"cereal::cereal"}},
        {"msgpack-cxx",           "msgpack-cxx",              {"msgpack-cxx"}},

        // CLI / 终端 UI（除已有 cli11/cxxopts）
        {"argparse",              "argparse",                 {"argparse::argparse"}},
        {"tabulate",              "tabulate",                 {"tabulate::tabulate"}},
        {"indicators",            "indicators",               {"indicators::indicators"}},
        {"ftxui",                 "ftxui",                    {"ftxui::component", "ftxui::dom", "ftxui::screen"}},
        {"termcolor",             "termcolor",                {"termcolor::termcolor"}},

        // 并发 / 任务
        {"concurrentqueue",       "unofficial-concurrentqueue", {"unofficial::concurrentqueue::concurrentqueue"}},
        {"taskflow",              "Taskflow",                 {"Taskflow::Taskflow"}},

        // 编译期工具
        {"magic-enum",            "magic_enum",               {"magic_enum::magic_enum"}},
        {"ctre",                  "ctre",                     {"ctre::ctre"}},
        {"frozen",                "frozen",                   {"frozen::frozen"}},

        // I/O 解析
        {"scnlib",                "scn",                      {"scn::scn"}},
        {"cpp-peglib",            "cpp-peglib",               {"cpp-peglib::cpp-peglib"}},
        {"tao-pegtl",             "pegtl",                    {"taocpp::pegtl"}},

        // 资产 / 引擎
        {"assimp",                "assimp",                   {"assimp::assimp"}},
        {"bullet3",               "Bullet",                   {"BulletDynamics", "BulletCollision", "LinearMath"}},
        {"box2d",                 "box2d",                    {"box2d::box2d"}},
        {"lua",                   "Lua",                      {"lua"}},
        {"sol2",                  "sol2",                     {"sol2::sol2"}},

        // 哈希 / 字符串
        {"xxhash",                "unofficial-xxhash",        {"unofficial::xxhash::xxhash"}},
        {"utfcpp",                "utf8cpp",                  {"utf8cpp"}},

        // 反汇编 / 调试
        {"capstone",              "capstone",                 {"capstone::capstone"}},
        {"zydis",                 "Zydis",                    {"Zydis::Zydis"}},
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
