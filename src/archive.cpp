#include "archive.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include "miniz.h"

#include "log.hpp"

namespace luban::archive {

namespace {

bool ends_with(const std::string& s, std::string_view suffix) {
    return s.size() >= suffix.size()
        && std::equal(suffix.rbegin(), suffix.rend(), s.rbegin());
}

bool ends_with_ci(const std::string& s, std::string_view suffix) {
    if (s.size() < suffix.size()) return false;
    for (size_t i = 0; i < suffix.size(); ++i) {
        char a = s[s.size() - suffix.size() + i];
        char b = suffix[i];
        if (std::tolower(static_cast<unsigned char>(a)) !=
            std::tolower(static_cast<unsigned char>(b))) return false;
    }
    return true;
}

// 路径穿越检查：拒绝 ".." 段、绝对路径、根目录指代。
// Python 用 .resolve() 比较；这里直接看路径段。
bool is_unsafe_entry(const std::string& name) {
    if (name.empty()) return true;
    if (name.front() == '/' || name.front() == '\\') return true;
    if (name.size() >= 2 && name[1] == ':') return true;  // C:/ 之类
    // 切段，每段不能是 ".."
    size_t start = 0;
    while (start < name.size()) {
        size_t end = start;
        while (end < name.size() && name[end] != '/' && name[end] != '\\') ++end;
        std::string seg = name.substr(start, end - start);
        if (seg == "..") return true;
        if (end == name.size()) break;
        start = end + 1;
    }
    return false;
}

// 把 archive 写到 dest_root，每条 entry 在内安全检查后写盘。
std::expected<void, Error> extract_to(const fs::path& archive_path, const fs::path& dest_root) {
    mz_zip_archive zip{};
    std::string apath = archive_path.string();

    if (!mz_zip_reader_init_file(&zip, apath.c_str(), 0)) {
        return std::unexpected(Error{
            ErrorKind::Corrupt,
            "miniz: not a valid zip: " + apath,
        });
    }

    mz_uint nfiles = mz_zip_reader_get_num_files(&zip);
    std::error_code ec;
    fs::create_directories(dest_root, ec);

    for (mz_uint i = 0; i < nfiles; ++i) {
        char name_buf[1024];
        mz_zip_reader_get_filename(&zip, i, name_buf, sizeof(name_buf));
        std::string name = name_buf;

        if (is_unsafe_entry(name)) {
            mz_zip_reader_end(&zip);
            return std::unexpected(Error{
                ErrorKind::UnsafeEntry,
                "unsafe zip entry escapes dest: " + name,
            });
        }

        bool is_dir = mz_zip_reader_is_file_a_directory(&zip, i);
        fs::path target = dest_root / fs::path(name).make_preferred();

        if (is_dir) {
            fs::create_directories(target, ec);
            continue;
        }
        fs::create_directories(target.parent_path(), ec);

        // miniz 写文件到磁盘（zip64 大文件也走这条）
        std::string tpath = target.string();
        if (!mz_zip_reader_extract_to_file(&zip, i, tpath.c_str(), 0)) {
            mz_zip_reader_end(&zip);
            return std::unexpected(Error{
                ErrorKind::Io,
                "miniz: extract_to_file failed for " + name + " → " + tpath,
            });
        }

        // 注意：miniz 不带文件 mode；POSIX 下可执行位丢失（M3 跨平台时再补）
    }

    mz_zip_reader_end(&zip);
    return {};
}

// 扁平化：如果 staging 下只有一个目录（不算隐藏文件），把其内容上提一层。
void flatten_single_wrapper(const fs::path& dir) {
    std::error_code ec;
    std::vector<fs::path> entries;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        std::string n = e.path().filename().string();
        if (!n.empty() && n.front() == '.') continue;  // 跳过 .
        entries.push_back(e.path());
    }
    if (entries.size() != 1) return;
    if (!fs::is_directory(entries[0], ec)) return;

    fs::path wrapper = entries[0];
    // 把 wrapper 内每个孩子搬到 dir，避免覆盖冲突时清理已存在
    for (auto& child : fs::directory_iterator(wrapper, ec)) {
        fs::path target = dir / child.path().filename();
        if (fs::exists(target, ec)) {
            if (fs::is_directory(target, ec) && !fs::is_symlink(target, ec)) {
                fs::remove_all(target, ec);
            } else {
                fs::remove(target, ec);
            }
        }
        fs::rename(child.path(), target, ec);
        if (ec) {
            // 跨卷 fallback
            std::error_code ec2;
            fs::copy(child.path(), target, fs::copy_options::recursive, ec2);
            fs::remove_all(child.path(), ec2);
            ec.clear();
        }
    }
    fs::remove_all(wrapper, ec);
}

}  // namespace

std::expected<void, Error> extract(const fs::path& archive_path, const fs::path& dest_dir) {
    std::error_code ec;
    if (!fs::exists(archive_path, ec)) {
        return std::unexpected(Error{ErrorKind::Io, "archive not found: " + archive_path.string()});
    }

    // 拒绝 .7z / .exe 自解压（Python 端同行为）
    std::string lname = archive_path.filename().string();
    std::transform(lname.begin(), lname.end(), lname.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ends_with(lname, ".7z") || ends_with(lname, ".exe") || ends_with(lname, ".msi")) {
        return std::unexpected(Error{
            ErrorKind::Unsupported,
            archive_path.filename().string() + ": 7z/msi/self-extracting EXE not supported. "
            "Provide an overlay manifest pointing to .zip or .tar.gz."
        });
    }

    // M2 仅支持 ZIP（与 Python MVP 一致；几乎所有 toolchain 都是 zip）
    bool is_zip_ext = ends_with_ci(lname, ".zip");
    // 也兼容 zip 内容但扩展名不是 .zip 的情况——交给 miniz 头探测决定
    if (!is_zip_ext) {
        // 试探：mz_zip_reader_init_file 失败就报 Unsupported
        mz_zip_archive probe{};
        if (!mz_zip_reader_init_file(&probe, archive_path.string().c_str(), 0)) {
            return std::unexpected(Error{
                ErrorKind::Unsupported,
                archive_path.filename().string() + ": only .zip supported in M2 (tar.gz/xz on the roadmap)",
            });
        }
        mz_zip_reader_end(&probe);
    }

    fs::create_directories(dest_dir.parent_path(), ec);

    // staging：dest_dir 同级临时目录，miniz 抽到这；成功后再合并
    fs::path staging = dest_dir.parent_path() /
        (dest_dir.filename().string() + ".luban-staging");
    if (fs::exists(staging, ec)) fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);

    if (auto rc = extract_to(archive_path, staging); !rc) {
        fs::remove_all(staging, ec);
        return std::unexpected(rc.error());
    }

    // 扁平化单顶层 wrapper
    flatten_single_wrapper(staging);

    // 把 staging/* 移到 dest_dir/*
    fs::create_directories(dest_dir, ec);
    for (auto& child : fs::directory_iterator(staging, ec)) {
        fs::path target = dest_dir / child.path().filename();
        if (fs::exists(target, ec)) {
            if (fs::is_directory(target, ec) && !fs::is_symlink(target, ec)) {
                fs::remove_all(target, ec);
            } else {
                fs::remove(target, ec);
            }
        }
        fs::rename(child.path(), target, ec);
        if (ec) {
            std::error_code ec2;
            fs::copy(child.path(), target, fs::copy_options::recursive, ec2);
            fs::remove_all(child.path(), ec2);
            ec.clear();
        }
    }
    fs::remove_all(staging, ec);
    return {};
}

}  // namespace luban::archive
