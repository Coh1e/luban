#include "cli.hpp"

namespace luban::commands {
void register_doctor();
void register_env();
void register_new();
void register_build();
void register_shim();
void register_add();
void register_remove();
void register_sync();
void register_target();
void register_which();
void register_search();
void register_run();
void register_describe();
void register_self();
void register_completion();
void register_doc();
void register_check();
void register_test();
void register_clean();
void register_fmt();
void register_tree();
void register_update();
void register_outdated();
void register_blueprint();
void register_migrate();
}  // namespace luban::commands

int main(int argc, char** argv) {
    luban::commands::register_doctor();
    luban::commands::register_env();
    luban::commands::register_new();
    luban::commands::register_build();
    luban::commands::register_shim();
    luban::commands::register_add();
    luban::commands::register_remove();
    luban::commands::register_sync();
    luban::commands::register_target();
    luban::commands::register_which();
    luban::commands::register_search();
    luban::commands::register_run();
    luban::commands::register_describe();
    luban::commands::register_self();
    luban::commands::register_completion();
    luban::commands::register_doc();
    luban::commands::register_check();
    luban::commands::register_test();
    luban::commands::register_clean();
    luban::commands::register_fmt();
    luban::commands::register_tree();
    luban::commands::register_update();
    luban::commands::register_outdated();
    luban::commands::register_blueprint();
    luban::commands::register_migrate();
    return luban::cli::dispatch(argc, argv);
}
