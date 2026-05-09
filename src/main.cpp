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
void register_migrate();
}  // namespace luban::commands

int main(int argc, char** argv) {
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
    luban::commands::register_migrate();
    return luban::cli::dispatch(argc, argv);
}
