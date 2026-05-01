// `luban new {app|lib} <name>` — scaffold from templates/.
// Mirrors luban_boot/commands/new.py.

#include <cstdio>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

#include "luban/embedded_help/new.hpp"  // luban::embedded_help::new_help

#include "../cli.hpp"
#include "../log.hpp"

namespace luban::commands {
// 在 commands/build_project.cpp 里实现的便捷函数。
int build_project(const std::filesystem::path& project_dir);
}

namespace luban::commands {

namespace {

namespace fs = std::filesystem;

bool valid_name(const std::string& name) {
    static const std::regex re(R"(^[a-z][a-z0-9_-]*$)");
    return std::regex_match(name, re);
}

// Locate the templates/ directory relative to the running luban.exe.
// Search order:
//   <exe_dir>/templates/<kind>            (installed layout — future)
//   <exe_dir>/../templates/<kind>         (uncommon)
//   <repo_root>/templates/<kind>          (build/default/luban.exe → repo)
fs::path find_template_root(std::string_view kind) {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4];
    DWORD got = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    fs::path exe = (got == 0) ? fs::current_path() / "luban.exe" : fs::path(std::wstring(buf, got));
#else
    fs::path exe = fs::current_path() / "luban";
#endif
    fs::path exe_dir = exe.parent_path();
    std::vector<fs::path> candidates = {
        exe_dir / "templates" / std::string(kind),
        exe_dir.parent_path() / "templates" / std::string(kind),
        exe_dir.parent_path().parent_path() / "templates" / std::string(kind),
    };
    std::error_code ec;
    for (auto& c : candidates) {
        if (fs::is_directory(c, ec)) return c;
    }
    return {};
}

std::string expand(std::string text, const std::map<std::string, std::string>& ctx) {
    for (auto& [k, v] : ctx) {
        std::string needle = "{{" + k + "}}";
        size_t pos = 0;
        while ((pos = text.find(needle, pos)) != std::string::npos) {
            text.replace(pos, needle.size(), v);
            pos += v.size();
        }
    }
    return text;
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    // Strip UTF-8 BOM (templates are saved with BOM).
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF
        && static_cast<unsigned char>(s[1]) == 0xBB
        && static_cast<unsigned char>(s[2]) == 0xBF) {
        s.erase(0, 3);
    }
    return s;
}

void write_file(const fs::path& p, const std::string& content) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

// 把 `{{name}}` 之类的占位符也在路径组件里展开。
// 模板里 `templates/app/src/{{name}}/main.cpp.tmpl` → 渲染时变成 `dst/src/<name>/main.cpp`。
fs::path expand_path(const fs::path& p, const std::map<std::string, std::string>& ctx) {
    std::string s = p.generic_string();
    s = expand(std::move(s), ctx);
    return fs::path(s);
}

int materialize(const fs::path& src_dir, const fs::path& dst_dir,
                const std::map<std::string, std::string>& ctx) {
    int n = 0;
    for (auto& entry : fs::recursive_directory_iterator(src_dir)) {
        if (!entry.is_regular_file()) continue;
        fs::path rel = fs::relative(entry.path(), src_dir);
        bool is_template = (rel.extension() == ".tmpl");
        if (is_template) rel.replace_extension();  // strip ".tmpl"

        // 路径里也展开 {{name}} → name
        fs::path expanded_rel = expand_path(rel, ctx);
        fs::path dst = dst_dir / expanded_rel;
        if (is_template) {
            std::string text = read_file(entry.path());
            write_file(dst, expand(std::move(text), ctx));
        } else {
            std::error_code ec;
            fs::create_directories(dst.parent_path(), ec);
            fs::copy_file(entry.path(), dst, fs::copy_options::overwrite_existing, ec);
        }
        ++n;
    }
    return n;
}

int run_new(const cli::ParsedArgs& args) {
    if (args.positional.size() < 2) {
        log::err("usage: luban new {app|lib} <name>");
        return 2;
    }
    const std::string& kind = args.positional[0];
    const std::string& name = args.positional[1];

    if (kind != "app" && kind != "lib") {
        log::errf("unsupported kind: {}", kind);
        return 2;
    }
    if (kind == "lib") log::warn("lib templates are not differentiated yet; using app template.");
    if (!valid_name(name)) {
        log::errf("invalid project name '{}'. Use lowercase letters, digits, '-' and '_'; must start with a letter.", name);
        return 2;
    }

    // --target=wasm picks templates/wasm-app/ (sets emscripten-friendly luban.cmake +
    // CMakePresets). Default = native (templates/app/).
    std::string target = "native";
    {
        auto it = args.opts.find("target");
        if (it != args.opts.end() && !it->second.empty()) target = it->second;
    }
    std::string template_kind;
    if (target == "native") {
        template_kind = "app";
    } else if (target == "wasm") {
        template_kind = "wasm-app";
    } else {
        log::errf("unknown --target '{}'. Supported: native, wasm.", target);
        return 2;
    }

    fs::path src = find_template_root(template_kind);
    if (src.empty()) {
        log::errf("template '{}' not found (looked relative to luban.exe and repo root)", template_kind);
        return 1;
    }

    fs::path parent_arg = args.opts.count("at") ? fs::path(args.opts.at("at")) : fs::current_path();
    std::error_code ec;
    fs::path parent = fs::absolute(parent_arg, ec);
    fs::path dst = parent / name;

    if (fs::exists(dst, ec)) {
        bool empty = true;
        for (auto& _ : fs::directory_iterator(dst, ec)) { (void)_; empty = false; break; }
        if (!empty) { log::errf("target {} already exists and is not empty", dst.string()); return 1; }
    }

    log::stepf("Scaffolding {} '{}' at {}", kind, name, dst.string());
    fs::create_directories(dst, ec);
    int n = materialize(src, dst, {{"name", name}});
    log::okf("wrote {} files", n);

    // Scenario 2 硬验收线（plan §0）：末尾自动跑一次 luban build，
    // 这样 nvim / vscode 第一次打开就有 compile_commands.json 可吃。
    // hello-world 没 vcpkg deps，build_project 自动选 no-vcpkg preset，~3-5s。
    bool no_build = false;
    auto it_nb = args.flags.find("no-build");
    if (it_nb != args.flags.end() && it_nb->second) no_build = true;

    // wasm target's auto-build needs `emcmake cmake --preset wasm` plumbing
    // which build_project handles when it sees the wasm preset shape. Done.

    if (no_build) {
        log::infof("next: cd {} && luban build", name);
    } else {
        log::stepf("running initial `luban build` to produce compile_commands.json");
        int rc = build_project(dst);
        if (rc != 0) {
            log::warnf("initial build failed (rc={}); cd {} && luban build to retry", rc, name);
        } else {
            log::infof("next: cd {} && nvim src/{}/main.cpp  (clangd ready out of the box)", name, name);
        }
    }
    return 0;
}

}  // namespace

void register_new() {
    cli::Subcommand c;
    c.name = "new";
    c.help = "scaffold a new C++ project (CMakeLists + vcpkg.json + luban.cmake)";
    c.group = "project";
    // Manual-grade --help: `docs/src/commands/new.md` is embedded at build
    // time (cmake/embed_text.cmake). Single source of truth for the mdBook
    // chapter and the in-binary --help.
    c.long_help = embedded_help::new_help;
    c.opts = {{"at", "."}, {"target", "native"}};
    c.flags = {"no-build"};
    c.n_positional = 2;
    c.positional_names = {"kind", "name"};
    c.examples = {
        "luban new app hello\tScaffold + auto-build (~3s, no deps)",
        "luban new lib mylib\tSame, but lib instead of executable",
        "luban new app foo --no-build\tSkip the initial build",
        "luban new app foo --at C:\\projects\tParent directory override",
        "luban new app foo --target wasm\tEmscripten C++ \xe2\x86\x92 .html/.js/.wasm output (requires `luban setup --with emscripten`)",
    };
    c.run = run_new;
    cli::register_subcommand(std::move(c));
}

}  // namespace luban::commands
