// `luban shim` — regenerate every shim under <data>/bin/ from the registry.
// Used to repair corrupt shims; M2's setup will also call write_shim during install.

#include <filesystem>

#include "../cli.hpp"
#include "../log.hpp"
#include "../paths.hpp"
#include "../registry.hpp"
#include "../shim.hpp"

namespace luban::commands {

namespace {

namespace fs = std::filesystem;

int run_shim(const cli::ParsedArgs&) {
    auto recs = registry::load_installed();
    if (recs.empty()) {
        log::warn("registry is empty \xe2\x80\x94 nothing to shim. Run setup first.");
        return 0;
    }
    int total = 0;
    for (auto& [name, rec] : recs) {
        fs::path root = paths::toolchain_dir(rec.toolchain_dir);
        for (auto& [alias, rel] : rec.bins) {
            std::string norm = rel;
            for (auto& c : norm) if (c == '/' || c == '\\') c = static_cast<char>(fs::path::preferred_separator);
            fs::path exe = root / norm;
            shim::write_shim(alias, exe);
            ++total;
        }
    }
    log::okf("regenerated shims for {} alias(es) under {}",
             total, paths::bin_dir().string());
    return 0;
}

}  // namespace

void register_shim() {
    cli::Subcommand c;
    c.name = "shim";
    c.help = "regenerate <data>/bin/ shims from installed.json (repair tool)";
    c.group = "advanced";
    c.long_help =
        "  Walk every component in installed.json and rewrite its shim files\n"
        "  (.cmd, .ps1, extensionless sh) under <data>/bin/. Useful when:\n"
        "    - shim dir was deleted\n"
        "    - toolchain dir was moved / re-bootstrapped\n"
        "    - a new luban version changed shim format";
    c.examples = {
        "luban shim\tRepair all shims",
    };
    c.run = run_shim;
    cli::register_subcommand(std::move(c));
}

}  // namespace luban::commands
