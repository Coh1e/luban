// See `iso_time.hpp`.

#include "iso_time.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace luban::iso_time {

std::string now() {
    using namespace std::chrono;
    auto t = system_clock::to_time_t(system_clock::now());
    std::tm gm{};
#ifdef _WIN32
    gmtime_s(&gm, &t);
#else
    gmtime_r(&t, &gm);
#endif
    std::ostringstream ss;
    ss << std::put_time(&gm, "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

}  // namespace luban::iso_time
