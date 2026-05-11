#include <curl/curl.h>

#include "cli.hpp"

namespace luban::commands {
void register_doctor();
void register_env();
void register_new();
void register_build();
void register_add();
void register_remove();
void register_run();
void register_describe();
void register_self();
void register_blueprint();
void register_msvc();
}  // namespace luban::commands

namespace {

// RAII guard: curl_global_init at startup, curl_global_cleanup at exit.
// libcurl_backend also self-initializes via std::call_once (defensive),
// but doing it explicitly here keeps the lifecycle visible and ensures
// the cleanup runs before static destructors (some of which may transitively
// hold libcurl resources in tests / future plugins).
struct CurlGlobal {
    CurlGlobal()  { curl_global_init(CURL_GLOBAL_DEFAULT); }
    ~CurlGlobal() { curl_global_cleanup(); }
};

}  // namespace

int main(int argc, char** argv) {
    CurlGlobal curl_lifetime;

    luban::commands::register_doctor();
    luban::commands::register_env();
    luban::commands::register_new();
    luban::commands::register_build();
    luban::commands::register_add();
    luban::commands::register_remove();
    luban::commands::register_run();
    luban::commands::register_describe();
    luban::commands::register_self();
    luban::commands::register_blueprint();
    luban::commands::register_msvc();
    return luban::cli::dispatch(argc, argv);
}
