#include "cli.hpp"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <ranges>

#include "log.hpp"

namespace luban::cli {

namespace {

constexpr std::string_view kVersion = "0.1.2";

std::vector<Subcommand>& registry() {
    static std::vector<Subcommand> v;
    return v;
}

const Subcommand* find_cmd(std::string_view name) {
    for (const auto& c : registry()) {
        if (c.name == name) return &c;
    }
    return nullptr;
}

// 分组标题映射 — 顺序就是输出顺序
const std::vector<std::pair<std::string, std::string>>& group_order() {
    static const std::vector<std::pair<std::string, std::string>> v = {
        {"setup",    "Toolchain & environment (install once per machine)"},
        {"project",  "Per-project commands (run inside a project dir)"},
        {"dep",      "Dependency management (vcpkg.json + luban.cmake)"},
        {"advanced", "Advanced / diagnostic"},
    };
    return v;
}

void print_top_help() {
    std::cout <<
        "luban — Windows-first C++ toolchain manager + cmake/vcpkg auxiliary frontend.\n"
        "\n"
        "Usage:  luban [-V|--version] [-v|--verbose] <command> [args]\n"
        "        luban help [<topic>]                      tutorials & deep-dives\n"
        "\n";

    size_t w = 0;
    for (const auto& c : registry()) w = std::max(w, c.name.size());

    for (const auto& [group, header] : group_order()) {
        bool first_in_group = true;
        for (const auto& c : registry()) {
            if (c.group != group) continue;
            if (first_in_group) {
                std::cout << header << ":\n";
                first_in_group = false;
            }
            std::cout << "  " << c.name;
            for (size_t i = c.name.size(); i < w + 2; ++i) std::cout << ' ';
            std::cout << c.help << '\n';
        }
        if (!first_in_group) std::cout << '\n';
    }

    std::cout <<
        "Common workflows:\n"
        "  First-time machine setup:\n"
        "    luban setup            install LLVM-MinGW + cmake + ninja + vcpkg\n"
        "    luban env --user       register on HKCU PATH (rustup-style; one-time)\n"
        "\n"
        "  Start a new C++ project (no deps):\n"
        "    luban new app foo      scaffold + auto-build (compile_commands.json ready)\n"
        "    cd foo && nvim src/foo/main.cpp     clangd ready out of the box\n"
        "\n"
        "  Add a vcpkg library:\n"
        "    luban add fmt          edits vcpkg.json + regenerates luban.cmake\n"
        "    luban build            cmake fetches the dep via vcpkg manifest mode\n"
        "\n"
        "  Multi-target project:\n"
        "    luban target add lib mylib          scaffold src/mylib/{.h,.cpp,CMakeLists.txt}\n"
        "    (in src/foo/CMakeLists.txt) target_link_libraries(foo PRIVATE mylib)\n"
        "\n"
        "Run `luban <command> --help` or `luban help <topic>` for details.\n";
}

void print_cmd_help(const Subcommand& c) {
    std::cout << "Usage: luban " << c.name;
    for (const auto& p : c.positional_names) std::cout << ' ' << '<' << p << '>';
    if (!c.flags.empty() || !c.opts.empty()) std::cout << " [options]";
    std::cout << "\n";

    if (!c.long_help.empty()) {
        std::cout << '\n' << c.long_help << '\n';
    } else {
        std::cout << "\n  " << c.help << "\n";
    }

    if (!c.flags.empty()) {
        std::cout << "\nFlags:\n";
        for (const auto& f : c.flags) std::cout << "  --" << f << '\n';
    }
    if (!c.opts.empty()) {
        std::cout << "\nOptions:\n";
        for (const auto& [k, v] : c.opts) {
            std::cout << "  --" << k << " <value>";
            if (!v.empty()) std::cout << "  (default: " << v << ")";
            std::cout << '\n';
        }
    }
    if (!c.examples.empty()) {
        std::cout << "\nExamples:\n";
        for (const auto& ex : c.examples) {
            auto tab = ex.find('\t');
            if (tab == std::string::npos) {
                std::cout << "  " << ex << '\n';
            } else {
                std::cout << "  " << ex.substr(0, tab) << '\n';
                std::cout << "      " << ex.substr(tab + 1) << '\n';
            }
        }
    }
}

bool is_flag_or_opt(std::string_view s) { return s.size() >= 2 && s[0] == '-'; }

ParsedArgs parse_subargs(const Subcommand& c, std::span<char*> args, bool& help_wanted) {
    ParsedArgs out;
    for (const auto& f : c.flags) out.flags[f] = false;
    for (const auto& [k, v] : c.opts) out.opts[k] = v;

    // forward_rest: subcommand wants all argv after itself as positional, verbatim.
    // Used by `luban run <cmd> [args...]` so flags meant for the child cmd
    // (e.g. `--version`) don't get parsed by luban's CLI.
    // Only `luban run --help` (single arg, no other words) prints luban's help.
    if (c.forward_rest) {
        if (args.size() == 1 && (args[0] == std::string_view("--help") ||
                                 args[0] == std::string_view("-h"))) {
            help_wanted = true;
            return out;
        }
        for (auto* a : args) out.positional.emplace_back(a);
        return out;
    }

    for (size_t i = 0; i < args.size(); ++i) {
        std::string_view a = args[i];
        if (a == "--help" || a == "-h") { help_wanted = true; continue; }
        if (a.starts_with("--")) {
            std::string_view name = a.substr(2);
            auto eq = name.find('=');
            std::string_view val;
            if (eq != std::string_view::npos) {
                val = name.substr(eq + 1);
                name = name.substr(0, eq);
            }
            // Match against this subcommand's options/flags.
            auto opt_it = std::find_if(c.opts.begin(), c.opts.end(),
                                       [&](auto& p) { return p.first == name; });
            if (opt_it != c.opts.end()) {
                if (eq == std::string_view::npos) {
                    if (i + 1 >= args.size()) {
                        log::errf("missing value for --{}", name);
                        out.opts["__error"] = "1";
                        return out;
                    }
                    val = args[++i];
                }
                out.opts[std::string(name)] = std::string(val);
                continue;
            }
            if (std::find(c.flags.begin(), c.flags.end(), std::string(name)) != c.flags.end()) {
                out.flags[std::string(name)] = true;
                continue;
            }
            log::errf("unknown option: --{}", name);
            out.opts["__error"] = "1";
            return out;
        }
        if (is_flag_or_opt(a)) {
            log::errf("unknown short option: {}", a);
            out.opts["__error"] = "1";
            return out;
        }
        out.positional.emplace_back(a);
    }
    return out;
}

}  // namespace

void register_subcommand(Subcommand cmd) {
    registry().push_back(std::move(cmd));
}

int dispatch(int argc, char** argv) {
    if (argc < 2) {
        print_top_help();
        return 0;
    }
    std::span<char*> rest(argv + 1, static_cast<size_t>(argc - 1));

    // Pre-pass: -v/--verbose can sit before the command.
    std::vector<char*> filtered;
    for (auto* a : rest) {
        std::string_view s = a;
        if (s == "-v" || s == "--verbose") log::set_verbose(true);
        else filtered.push_back(a);
    }
    if (filtered.empty()) { print_top_help(); return 0; }

    std::string_view first = filtered[0];
    if (first == "-V" || first == "--version") {
        std::cout << "luban " << kVersion << '\n';
        return 0;
    }
    if (first == "-h" || first == "--help") {
        print_top_help();
        return 0;
    }

    const Subcommand* c = find_cmd(first);
    if (!c) {
        log::errf("unknown command: {}", first);
        print_top_help();
        return 2;
    }

    std::span<char*> sub(filtered.data() + 1, filtered.size() - 1);
    bool help_wanted = false;
    ParsedArgs parsed = parse_subargs(*c, sub, help_wanted);
    if (help_wanted) { print_cmd_help(*c); return 0; }
    if (parsed.opts.contains("__error")) return 2;
    if (static_cast<int>(parsed.positional.size()) < c->n_positional) {
        log::errf("expected {} positional argument(s) for `{}`, got {}",
                  c->n_positional, c->name, parsed.positional.size());
        print_cmd_help(*c);
        return 2;
    }
    return c->run(parsed);
}

}  // namespace luban::cli
