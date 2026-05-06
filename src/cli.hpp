#pragma once
// Tiny argparse — we ship this rather than pull in cxxopts.
// Subcommand dispatch + a few flag/option types is enough for M1.

#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace luban::cli {

struct ParsedArgs {
    // Boolean flags (--verbose, --apply).
    std::map<std::string, bool> flags;
    // Options with values (--preset default, --at .).
    std::map<std::string, std::string> opts;
    // Positionals (kind, name).
    std::vector<std::string> positional;
};

struct Subcommand {
    std::string name;
    std::string help;
    // 长描述（多行）—— 出现在 `luban <cmd> --help` 顶部
    std::string long_help;
    // 示例命令 + 解释。每条 "command\texplanation"。
    std::vector<std::string> examples;
    // 分组（top-level help 用）：setup / project / dep / verb / advanced
    std::string group = "advanced";
    // Flags this subcommand recognises (without the leading "--").
    std::vector<std::string> flags;
    // Options recognised — name → default value (used when missing).
    std::vector<std::pair<std::string, std::string>> opts;
    // Number of positionals expected (fixed).
    int n_positional = 0;
    std::vector<std::string> positional_names;
    // Handler: receives parsed args, returns process exit code.
    std::function<int(const ParsedArgs&)> run;
    // forward_rest 让 cli parser 不解析 flag/option，全收 positional（M3 run 用）。
    bool forward_rest = false;
};

// Registers a subcommand. Order of registration determines `--help` output.
void register_subcommand(Subcommand cmd);

// Top-level entry point — argv[0] is process name, argv[1] is subcommand or
// --version/--help.
int dispatch(int argc, char** argv);

}  // namespace luban::cli
