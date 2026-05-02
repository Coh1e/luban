// `luban target add {lib|exe} <name>` / `luban target remove <name>`
//
// 纯增量（plan §4 §1）：day-1 就 subdir 布局，所以 add 只是创建 src/<name>/
// + 更新 luban.cmake 的 LUBAN_TARGETS。

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

#include "../cli.hpp"
#include "../log.hpp"
#include "../luban_cmake_gen.hpp"

namespace luban::commands {

namespace {

namespace fs = std::filesystem;

bool valid_name(const std::string& name) {
    static const std::regex re(R"(^[a-z][a-z0-9_-]*$)");
    return std::regex_match(name, re);
}

fs::path find_project_root() {
    std::error_code ec;
    fs::path d = fs::current_path(ec);
    while (!d.empty()) {
        if (fs::exists(d / "luban.cmake", ec)) return d;
        fs::path parent = d.parent_path();
        if (parent == d) break;
        d = parent;
    }
    return fs::current_path(ec);
}

void write_text(const fs::path& path, const std::string& content) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

// 生成 src/<name>/CMakeLists.txt + 源文件骨架。
void scaffold_target_dir(const fs::path& project, const std::string& name, const std::string& kind) {
    fs::path tdir = project / "src" / name;
    std::error_code ec;
    fs::create_directories(tdir, ec);

    if (kind == "lib") {
        std::ostringstream cml;
        cml << "add_library(" << name << " STATIC " << name << ".cpp)\n"
            << "target_include_directories(" << name << " PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})\n"
            << "luban_apply(" << name << ")\n";
        write_text(tdir / "CMakeLists.txt", cml.str());

        std::ostringstream h;
        h << "#pragma once\n"
          << "namespace " << name << " { int hello(); }\n";
        write_text(tdir / (name + ".h"), h.str());

        std::ostringstream cpp;
        cpp << "#include \"" << name << ".h\"\n"
            << "namespace " << name << " { int hello() { return 42; } }\n";
        write_text(tdir / (name + ".cpp"), cpp.str());
    } else {
        // exe
        std::ostringstream cml;
        cml << "add_executable(" << name << " main.cpp)\n"
            << "luban_apply(" << name << ")\n";
        write_text(tdir / "CMakeLists.txt", cml.str());

        std::ostringstream main;
        main << "#include <print>\n\n"
             << "int main() {\n"
             << "    std::println(\"hello from " << name << "!\");\n"
             << "    return 0;\n"
             << "}\n";
        write_text(tdir / "main.cpp", main.str());
    }
}

int run_target(const cli::ParsedArgs& a) {
    if (a.positional.empty()) {
        log::err("usage: luban target add {lib|exe} <name>  |  luban target remove <name>");
        return 2;
    }
    const std::string& sub = a.positional[0];

    fs::path proj = find_project_root();
    if (!fs::exists(proj / "luban.cmake")) {
        log::errf("not in a luban project (no luban.cmake found from {})", fs::current_path().string());
        return 2;
    }

    auto targets = luban_cmake_gen::read_targets_from_cmake(proj);

    if (sub == "add") {
        if (a.positional.size() < 3) {
            log::err("usage: luban target add {lib|exe} <name>");
            return 2;
        }
        const std::string& kind = a.positional[1];
        const std::string& name = a.positional[2];
        if (kind != "lib" && kind != "exe") {
            log::errf("unsupported kind '{}' (use lib or exe)", kind);
            return 2;
        }
        if (!valid_name(name)) {
            log::errf("invalid target name '{}'", name);
            return 2;
        }
        if (std::find(targets.begin(), targets.end(), name) != targets.end()) {
            log::errf("target '{}' already exists in LUBAN_TARGETS", name);
            return 2;
        }
        std::error_code ec;
        if (fs::exists(proj / "src" / name, ec)) {
            log::errf("src/{} already exists; refusing to overwrite", name);
            return 2;
        }
        scaffold_target_dir(proj, name, kind);
        targets.push_back(name);
        luban_cmake_gen::regenerate_in_project(proj, targets);
        log::okf("created {} target '{}' at src/{}/", kind, name, name);
        log::okf("regenerated luban.cmake (LUBAN_TARGETS now: {})",
                 [&]{
                     std::string s;
                     for (size_t i = 0; i < targets.size(); ++i) { if (i) s += ", "; s += targets[i]; }
                     return s;
                 }());
        return 0;
    }

    if (sub == "remove") {
        if (a.positional.size() < 2) {
            log::err("usage: luban target remove <name>");
            return 2;
        }
        const std::string& name = a.positional[1];
        auto it = std::find(targets.begin(), targets.end(), name);
        if (it == targets.end()) {
            log::warnf("target '{}' not in LUBAN_TARGETS", name);
            return 0;
        }
        targets.erase(it);
        luban_cmake_gen::regenerate_in_project(proj, targets);
        log::okf("removed '{}' from LUBAN_TARGETS", name);
        std::error_code ec;
        if (fs::exists(proj / "src" / name, ec)) {
            log::warnf("src/{}/ left in place; rm manually if you really want it gone", name);
        }
        return 0;
    }

    log::errf("unknown target subcommand '{}' (use add or remove)", sub);
    return 2;
}

}  // namespace

void register_target() {
    cli::Subcommand c;
    c.name = "target";
    c.help = "add/remove a build target (lib or exe) within the current project";
    c.group = "project";
    c.long_help =
        "  `luban target add lib <name>` creates src/<name>/{<name>.h, <name>.cpp,\n"
        "                                       CMakeLists.txt} and registers it.\n"
        "  `luban target add exe <name>` creates src/<name>/{main.cpp, CMakeLists.txt}\n"
        "  `luban target remove <name>`  unregisters; src/<name>/ is left in place.\n"
        "\n"
        "  Inter-target linking is plain cmake — write\n"
        "      target_link_libraries(<exe> PRIVATE <lib>)\n"
        "  in src/<exe>/CMakeLists.txt yourself. Luban does not abstract this.";
    c.n_positional = 1;
    c.positional_names = {"sub", "kind?", "name?"};
    c.examples = {
        "luban target add lib mathlib\tNew static lib with mathlib::hello() stub",
        "luban target add exe bench\tNew executable target",
        "luban target remove temp\tUnregister (does NOT delete src/temp/)",
    };
    c.run = run_target;
    cli::register_subcommand(std::move(c));
}

}  // namespace luban::commands
