#include "cli.hpp"

namespace luban::commands {
void register_doctor();
void register_env();
void register_new();
void register_build();
void register_shim();
void register_setup();
void register_add();
void register_remove();
void register_sync();
void register_target();
void register_which();
void register_search();
void register_run();
void register_describe();
}  // namespace luban::commands

int main(int argc, char** argv) {
    luban::commands::register_doctor();
    luban::commands::register_env();
    luban::commands::register_new();
    luban::commands::register_build();
    luban::commands::register_shim();
    luban::commands::register_setup();
    luban::commands::register_add();
    luban::commands::register_remove();
    luban::commands::register_sync();
    luban::commands::register_target();
    luban::commands::register_which();
    luban::commands::register_search();
    luban::commands::register_run();
    luban::commands::register_describe();
    return luban::cli::dispatch(argc, argv);
}
