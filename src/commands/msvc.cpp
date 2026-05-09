// `luban msvc shell` / `luban msvc run -- <cmd>` — DESIGN §6.2.
//
// vcvarsall.bat sets ~30 env vars (INCLUDE, LIB, LIBPATH, PATH additions,
// VCToolsInstallDir, WindowsSdkDir, ...) that the MSVC toolchain needs.
// Sourcing it manually each session is the friction these verbs remove:
//
//   msvc shell           open a child pwsh with vcvars injected; exit
//                        cleanly returns you to the unmodified host shell.
//   msvc run -- <cmd>    exec <cmd> with vcvars injected; one-shot, no
//                        shell session.
//
// Neither writes to HKCU. Per DESIGN §6.2 these envs are session-only
// and never pollute the user's persistent environment.
//
// Capture is on-demand: if <state>/msvc-env.json doesn't exist (or the
// user did `luban env --msvc-clear`), we run vcvarsall once and persist
// the diff before injecting. Subsequent invocations reuse the cached
// snapshot.

#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "../cli.hpp"
#include "../log.hpp"
#include "../msvc_env.hpp"
#include "../proc.hpp"

namespace luban::commands {

namespace {

namespace fs = std::filesystem;

// Load the captured MSVC env, capturing fresh if no snapshot exists.
// Returns nullopt + logs on user-visible error (no VS install, capture
// failed). The caller stops on nullopt.
std::optional<luban::msvc_env::Captured> ensure_captured(const std::string& arch) {
    if (auto cached = luban::msvc_env::load(); cached) {
        // Re-capture if arch differs from cached (user passed --arch x86 but
        // cache was x64). Otherwise reuse.
        if (cached->arch == arch) return cached;
        log::infof("msvc: cached arch is {}, re-capturing for {}",
                   cached->arch, arch);
    }

    fs::path install = luban::msvc_env::find_install();
    if (install.empty()) {
        log::err("msvc: no Visual Studio / VS Build Tools install found.");
        log::info("hint: install MSBuild Tools from https://visualstudio.microsoft.com/downloads/");
        log::info("      (luban does NOT install MSVC — it's a per-machine prerequisite)");
        return std::nullopt;
    }
    log::infof("msvc: capturing vcvars from {} ({})", install.string(), arch);
    auto captured = luban::msvc_env::capture(install, arch);
    if (!captured) {
        log::err("msvc: vcvarsall capture failed (see prior errors)");
        return std::nullopt;
    }
    if (!luban::msvc_env::save(*captured)) {
        log::warn("msvc: failed to persist capture; will recapture next time");
    }
    return captured;
}

// Build the env_overrides map proc::run wants. Includes the captured vars
// + a PATH that prepends path_addition to the host PATH (so MSVC's tool
// dirs win, but the rest of PATH is preserved).
std::map<std::string, std::string> build_env_overrides(
    const luban::msvc_env::Captured& c) {
    std::map<std::string, std::string> env(c.vars.begin(), c.vars.end());
    if (!c.path_addition.empty()) {
        std::string cur;
        if (const char* p = std::getenv("PATH"); p) cur = p;
        env["PATH"] = c.path_addition + (cur.empty() ? "" : ";" + cur);
    }
    return env;
}

int run_shell(const cli::ParsedArgs& a) {
    std::string arch = "x64";
    if (auto it = a.opts.find("arch"); it != a.opts.end() && !it->second.empty()) {
        arch = it->second;
    }
    auto captured = ensure_captured(arch);
    if (!captured) return 1;

    auto env = build_env_overrides(*captured);

    // Default child shell: pwsh.exe (Windows-first project; pwsh is the
    // canonical workbench shell per cli-tools bp). Fall back to powershell
    // if pwsh isn't on PATH. -NoExit so the env stays alive after the
    // first command (interactive shell).
    std::vector<std::string> cmd = {"pwsh.exe", "-NoExit",
                                    "-NoProfile",  // skip profile churn for cleaner repro
                                    "-Command", "$env:LUBAN_MSVC_SHELL=1"};
    log::infof("msvc shell: spawning pwsh with MSVC env (arch={})", arch);
    log::info("  exit the child shell to return to the unmodified host shell");
    int rc = luban::proc::run(cmd, fs::current_path().string(), env);
    if (rc == -1) {
        log::err("msvc shell: pwsh.exe spawn failed (is PowerShell 7 installed?)");
        return 127;
    }
    return rc;
}

int run_msvc_run(const cli::ParsedArgs& a) {
    if (a.positional.empty()) {
        log::err("usage: luban msvc run -- <cmd> [args...]");
        log::info("example:  luban msvc run -- cl /?");
        return 2;
    }
    std::string arch = "x64";
    if (auto it = a.opts.find("arch"); it != a.opts.end() && !it->second.empty()) {
        arch = it->second;
    }
    auto captured = ensure_captured(arch);
    if (!captured) return 1;

    auto env = build_env_overrides(*captured);
    std::vector<std::string> cmd(a.positional.begin(), a.positional.end());
    int rc = luban::proc::run(cmd, fs::current_path().string(), env);
    if (rc == -1) {
        log::errf("msvc run: spawn failed for `{}`", cmd[0]);
        return 127;
    }
    return rc;
}

int run_msvc(const cli::ParsedArgs& args) {
    if (args.positional.empty()) {
        std::fprintf(stderr, "luban msvc: missing subcommand (shell | run)\n");
        return 2;
    }
    std::string sub = args.positional[0];
    cli::ParsedArgs rest = args;
    rest.positional.erase(rest.positional.begin());

    if (sub == "shell") return run_shell(rest);
    if (sub == "run")   return run_msvc_run(rest);

    std::fprintf(stderr, "luban msvc: unknown subcommand `%s` (shell | run)\n",
                 sub.c_str());
    return 2;
}

}  // namespace

void register_msvc() {
    cli::Subcommand c;
    c.name = "msvc";
    c.help = "MSVC vcvars-injected shell / one-shot exec (DESIGN §6.2)";
    c.group = "setup";
    c.long_help =
        "  `luban msvc <subcommand>` injects MSVC's vcvarsall.bat env\n"
        "  (INCLUDE / LIB / LIBPATH / PATH / ~30 others) into a child\n"
        "  process. The host shell stays clean — env never persists to\n"
        "  HKCU.\n\n"
        "  Subcommands:\n"
        "    shell             open a child pwsh.exe with vcvars injected\n"
        "    run -- <cmd>      one-shot exec; <cmd> sees the MSVC env\n\n"
        "  Options:\n"
        "    --arch <a>        x64 (default) | x86 | arm64\n\n"
        "  First invocation runs vcvarsall and caches the env diff at\n"
        "  <state>/msvc-env.json. Subsequent invocations reuse the cache.\n"
        "  Re-capture with `luban env --msvc-clear` then re-run.\n\n"
        "  Requires Visual Studio / VS Build Tools installed (luban does\n"
        "  not install MSVC).";
    c.opts = {{"arch", ""}};
    c.forward_rest = true;  // for `msvc run -- foo --bar`
    c.examples = {
        "luban msvc shell\tOpen pwsh with MSVC env",
        "luban msvc run -- cl /?\tOne-shot: print cl.exe usage",
        "luban msvc run --arch arm64 -- cmake --build .\tBuild with arm64 toolchain",
    };
    c.run = run_msvc;
    cli::register_subcommand(std::move(c));
}

}  // namespace luban::commands
