// See `file_deploy.hpp`.
//
// Backup naming uses a base64-url-style encoding of the target path.
// Why: backup dir entries need to be filename-safe and reversible. The
// usual hash-of-path approach is filename-safe but lossy — debugging an
// "what was this backup?" requires lookup. Base64 of the absolute path
// is reversible, filename-safe (after we substitute / for _ and \ for
// __), and free of platform-special chars.
//
// Atomic write reuses file_util::write_text_atomic which itself does
// tmp + rename, so an interrupted deploy never leaves a half-written
// target.

#include "file_deploy.hpp"

#include <cstdint>
#include <fstream>
#include <sstream>
#include <system_error>

#include "file_util.hpp"
#include "hash.hpp"
#include "paths.hpp"

namespace luban::file_deploy {

namespace {

namespace fs = std::filesystem;
namespace bp = luban::blueprint;

/// Base64 encoder using the URL-safe alphabet (no padding). Used to
/// turn an arbitrary file path into a filename-safe backup directory
/// entry. `path_to_backup_name` further escapes / and \ since on
/// Windows those would otherwise become subdirectories.
std::string base64url_encode(std::string_view in) {
    static const char* alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    int val = 0;
    int valb = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(alphabet[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) {
        out.push_back(alphabet[((val << 8) >> (valb + 8)) & 0x3F]);
    }
    return out;
}

std::string path_to_backup_name(const fs::path& target) {
    return base64url_encode(target.string());
}

fs::path backup_dir(int generation_id) {
    return paths::state_dir() / "backups" / std::to_string(generation_id);
}

/// `<state>/file-store/<sha>/content` — content-addressed storage for
/// deployed file bytes, so rollback can recreate files even after the
/// originating blueprint is unapplied. Sibling to `<state>/store/` (tool
/// artifacts) but separate because deployed files are typically small
/// inline strings while artifacts are zip archives — different lifecycle,
/// different gc story.
fs::path content_store_dir_for(std::string_view sha) {
    return paths::state_dir() / "file-store" / std::string(sha);
}

/// sha256 of an in-memory string. Used to record what we wrote into a
/// DeployedFile for later drift detection.
///
/// Writes the content to a temp file (so we can reuse hash::hash_file
/// rather than rolling an in-memory hasher) and removes it after. We
/// use `fs::temp_directory_path()` instead of `paths::cache_dir()`
/// because it's OS-guaranteed to exist; cache_dir might not yet on a
/// fresh install.
std::string sha256_of_content(std::string_view content) {
    auto tmp = fs::temp_directory_path() / ".luban-content-hash.tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return "";
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.close();
    }
    auto h = hash::hash_file(tmp, hash::Algorithm::Sha256);
    std::error_code ec;
    fs::remove(tmp, ec);
    if (!h) return "";
    return h->hex;
}

}  // namespace

fs::path expand_home(std::string_view raw) {
    // ~/foo  → <home>/foo
    // ~/     → <home>
    // ~      → <home>
    // anything else → unchanged
    if (raw == "~") return paths::home();
    if (raw.size() >= 2 && raw[0] == '~' && (raw[1] == '/' || raw[1] == '\\')) {
        auto rest = raw.substr(2);
        return paths::home() / std::string(rest);
    }
    // ~user/path is POSIX-only and we don't bother resolving it; passes
    // through verbatim. If someone needs it they can write the absolute
    // path in the blueprint.
    return fs::path(std::string(raw));
}

std::expected<DeployedFile, std::string> deploy(const bp::FileSpec& spec,
                                                int generation_id) {
    fs::path target = expand_home(spec.target_path);

    DeployedFile out;
    out.target_path = target;
    out.mode = spec.mode;
    out.content_sha256 = sha256_of_content(spec.content);

    // Snapshot deployed bytes into the content store, keyed by sha. Used
    // by blueprint_reconcile to recreate files when rolling back across
    // an unapply step (the backup chain only preserves *prior* content,
    // never the bytes we're about to write here). Idempotent: same sha
    // means same bytes, so an existing file is left alone.
    if (!out.content_sha256.empty()) {
        std::error_code cs_ec;
        auto cs_dir = content_store_dir_for(out.content_sha256);
        fs::create_directories(cs_dir, cs_ec);
        if (cs_ec) {
            return std::unexpected("cannot create content store dir " +
                                   cs_dir.string() + ": " + cs_ec.message());
        }
        auto cs_path = cs_dir / "content";
        if (!fs::exists(cs_path, cs_ec)) {
            if (!file_util::write_text_atomic(cs_path, spec.content)) {
                return std::unexpected("write failed: " + cs_path.string());
            }
        }
    }

    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    if (ec) {
        return std::unexpected("cannot create parent dir of " +
                               target.string() + ": " + ec.message());
    }

    // Replace mode: back up an existing target before writing. Drop-in
    // mode never touches the canonical user-owned file, so backup is
    // unnecessary even when the drop-in subfile already exists (luban
    // owns drop-in subfiles by definition).
    if (spec.mode == bp::FileMode::Replace && fs::exists(target, ec)) {
        auto backups = backup_dir(generation_id);
        fs::create_directories(backups, ec);
        if (ec) {
            return std::unexpected("cannot create backup dir: " + ec.message());
        }
        auto backup = backups / path_to_backup_name(target);
        fs::copy_file(target, backup,
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            return std::unexpected("cannot back up " + target.string() +
                                   " to " + backup.string() + ": " +
                                   ec.message());
        }
        out.backup_path = backup;
    }

    // Atomic content write. file_util::write_text_atomic uses tmp +
    // rename so we don't leave half-written files on disk.
    if (!file_util::write_text_atomic(target, spec.content)) {
        return std::unexpected("write failed: " + target.string());
    }
    return out;
}

std::expected<void, std::string> restore(const DeployedFile& deployed) {
    std::error_code ec;
    if (deployed.backup_path.has_value()) {
        // Original existed at deploy time → put it back.
        fs::copy_file(*deployed.backup_path, deployed.target_path,
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            return std::unexpected("restore from backup failed: " + ec.message());
        }
    } else {
        // No original at deploy time → remove our deployed copy. Don't
        // care if it's already gone (idempotent rollback).
        fs::remove(deployed.target_path, ec);
        // Note: ec on remove for nonexistent target is not an error
        // condition we care about.
    }
    return {};
}

fs::path content_store_path(std::string_view sha256_hex) {
    return content_store_dir_for(sha256_hex) / "content";
}

std::expected<void, std::string> recreate_from_content_store(
    std::string_view sha256_hex, const fs::path& target_path) {
    auto src = content_store_path(sha256_hex);
    std::error_code ec;
    if (!fs::is_regular_file(src, ec)) {
        return std::unexpected("content store miss for sha256:" +
                               std::string(sha256_hex) + " at " + src.string());
    }
    fs::create_directories(target_path.parent_path(), ec);
    if (ec) {
        return std::unexpected("cannot create parent dir of " +
                               target_path.string() + ": " + ec.message());
    }
    fs::copy_file(src, target_path, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        return std::unexpected("recreate " + target_path.string() +
                               " from content store: " + ec.message());
    }
    return {};
}

}  // namespace luban::file_deploy
