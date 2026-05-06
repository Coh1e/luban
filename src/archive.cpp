#include "archive.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <thread>
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

// Decide how many worker threads to use for parallel ZIP extraction.
// llvm-mingw (~270 small files in nested dirs) profiles at 154 s
// single-threaded vs ~30 s with 4 workers on the same disk; the speedup
// is from CPU-bound DEFLATE running in parallel, not disk I/O. Cap at 8
// because miniz's per-thread state (open file handle + decompressor)
// is small but not free, and SSDs saturate around 4-6 concurrent writes.
//
// `LUBAN_EXTRACT_THREADS` lets users override (0 = single-threaded).
int extract_thread_count() {
    if (const char* env = std::getenv("LUBAN_EXTRACT_THREADS")) {
        try {
            int n = std::stoi(env);
            if (n < 0) n = 0;
            if (n > 32) n = 32;
            return n;
        } catch (...) {}
    }
    int hw = static_cast<int>(std::thread::hardware_concurrency());
    if (hw <= 0) return 4;
    if (hw < 4) return hw;
    return std::min(8, hw);
}

// One pre-pass on the main thread: read the central directory, validate
// entry safety, materialize every directory, and build the work list of
// regular-file entries. Workers then handle the actual decompression in
// parallel — each opens its own mz_zip_archive on the same archive path
// (miniz's reader handles aren't thread-safe when shared).
std::expected<void, Error> extract_to(const fs::path& archive_path,
                                      const fs::path& dest_root,
                                      const ProgressCb& on_progress) {
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

    struct Item {
        mz_uint idx;
        std::string name;       // original zip-entry path (for error msgs)
        std::string out_path;   // resolved target on disk (UTF-8 string)
    };
    std::vector<Item> items;
    items.reserve(nfiles);

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
        items.push_back(Item{i, std::move(name), target.string()});
    }

    mz_zip_reader_end(&zip);

    int n_threads = extract_thread_count();
    // Below ~50 entries the cost of spinning up workers + opening N
    // mz_zip_archive handles dwarfs the savings, so fall back to
    // single-threaded for small archives. ninja-win.zip lands here.
    if (n_threads < 2 || items.size() < 50) {
        mz_zip_archive z{};
        if (!mz_zip_reader_init_file(&z, apath.c_str(), 0)) {
            return std::unexpected(Error{
                ErrorKind::Corrupt, "miniz: re-open failed: " + apath});
        }
        size_t done_st = 0;
        for (auto& it : items) {
            if (!mz_zip_reader_extract_to_file(&z, it.idx, it.out_path.c_str(), 0)) {
                mz_zip_reader_end(&z);
                return std::unexpected(Error{
                    ErrorKind::Io,
                    "miniz: extract_to_file failed for " + it.name + " → " + it.out_path});
            }
            ++done_st;
            if (on_progress) on_progress(done_st, items.size());
        }
        mz_zip_reader_end(&z);
        return {};
    }

    std::atomic<size_t> next_idx{0};
    std::atomic<size_t> done_count{0};
    std::atomic<bool> aborted{false};
    std::mutex err_mu;
    std::optional<Error> first_err;
    const size_t total_items = items.size();

    auto worker = [&]() {
        mz_zip_archive z{};
        if (!mz_zip_reader_init_file(&z, apath.c_str(), 0)) {
            std::lock_guard<std::mutex> g(err_mu);
            if (!first_err) first_err = Error{
                ErrorKind::Corrupt, "miniz: per-thread re-open failed: " + apath};
            aborted = true;
            return;
        }
        while (!aborted.load(std::memory_order_relaxed)) {
            size_t i = next_idx.fetch_add(1, std::memory_order_relaxed);
            if (i >= items.size()) break;
            const auto& it = items[i];
            if (!mz_zip_reader_extract_to_file(&z, it.idx, it.out_path.c_str(), 0)) {
                std::lock_guard<std::mutex> g(err_mu);
                if (!first_err) first_err = Error{
                    ErrorKind::Io,
                    "miniz: extract_to_file failed for " + it.name + " → " + it.out_path};
                aborted = true;
                break;
            }
            // Progress fires from any worker; cb is responsible for its
            // own thread-safety + frame throttling. Cheap to call when
            // disabled (empty std::function bool-test).
            size_t d = done_count.fetch_add(1, std::memory_order_relaxed) + 1;
            if (on_progress) on_progress(d, total_items);
        }
        mz_zip_reader_end(&z);
    };

    std::vector<std::thread> threads;
    threads.reserve(n_threads);
    for (int t = 0; t < n_threads; ++t) threads.emplace_back(worker);
    for (auto& t : threads) t.join();

    if (first_err) return std::unexpected(*first_err);
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

std::expected<void, Error> extract(const fs::path& archive_path,
                                   const fs::path& dest_dir,
                                   const ProgressCb& on_progress) {
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

    if (auto rc = extract_to(archive_path, staging, on_progress); !rc) {
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
