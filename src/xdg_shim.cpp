// See `xdg_shim.hpp`.
//
// Code lifted from blueprint_apply.cpp's anonymous-namespace helpers so
// blueprint_reconcile (rollback) can rebuild shims it deletes. The format
// matches what blueprint_apply has been emitting since v1.0:
//
//   Windows: `@echo off\r\n"<exe>" %*\r\n`  (CRLF line endings — cmd.exe
//            is happier with them and src/shim.cpp standardized on CRLF
//            in v0.x for the same reason).
//   POSIX  : symlink at `<bin>/<alias>` pointing at `<exe>`. Falls back to
//            a plain copy + chmod when the filesystem refuses symlinks
//            (rare — FAT-style mounts).

#include "xdg_shim.hpp"

#include <fstream>
#include <system_error>

#include "paths.hpp"

namespace luban::xdg_shim {

std::expected<fs::path, std::string> write_cmd_shim(
    std::string_view alias, const fs::path& exe) {
    fs::path bin = paths::xdg_bin_home();
    std::error_code ec;
    fs::create_directories(bin, ec);
    if (ec) {
        return std::unexpected("cannot create " + bin.string() + ": " + ec.message());
    }

#ifdef _WIN32
    fs::path shim_path = bin / (std::string(alias) + ".cmd");
    std::ofstream out(shim_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return std::unexpected("cannot open " + shim_path.string());
    }
    // Native separators only — mixing '/' into a .cmd path here works for
    // cmd.exe but rots when downstream tools (PowerShell completion,
    // some SDK probes) parse the file. make_preferred() forces '\\'.
    fs::path exe_native = exe;
    exe_native.make_preferred();
    out << "@echo off\r\n\"" << exe_native.string() << "\" %*\r\n";
    if (!out) {
        return std::unexpected("write failure on " + shim_path.string());
    }
    return shim_path;
#else
    fs::path shim_path = bin / std::string(alias);
    fs::remove(shim_path, ec);
    fs::create_symlink(exe, shim_path, ec);
    if (!ec) return shim_path;

    ec.clear();
    fs::copy_file(exe, shim_path, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        return std::unexpected("cannot create shim at " + shim_path.string() +
                               ": " + ec.message());
    }
    fs::permissions(shim_path,
                    fs::perms::owner_all | fs::perms::group_read |
                        fs::perms::group_exec | fs::perms::others_read |
                        fs::perms::others_exec,
                    fs::perm_options::add, ec);
    return shim_path;
#endif
}

std::expected<void, std::string> remove_cmd_shim(const fs::path& shim_path) {
    std::error_code ec;
    fs::remove(shim_path, ec);
    // Missing → not an error (idempotent). ec might also be set on FS-level
    // failures (permissions); surface those.
    if (ec) {
        return std::unexpected("cannot remove " + shim_path.string() +
                               ": " + ec.message());
    }
#ifndef _WIN32
    // POSIX symlinks land at <bin>/<alias> (no extension). On Windows the
    // recorded path is the .cmd; both directions handled above.
#endif
    return {};
}

}  // namespace luban::xdg_shim
