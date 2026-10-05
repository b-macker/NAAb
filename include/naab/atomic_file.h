// NAAb atomic file replacement for the compile caches.
//
// Two naab-lang processes compiling the same block install the same cache
// file. Writing it in place -- copy_file(..., overwrite_existing) or an
// ofstream on the final name -- truncates the file first, so for the length
// of the write any other reader sees a partial file, and a process that has
// already dlopen()ed it has its mapping cut out from under it (SIGBUS on the
// next page it touches).
//
// These write a uniquely named file in the destination's OWN directory and
// rename() it over the destination. Same directory means same filesystem, so
// the rename is atomic: a reader opens either the old inode or the complete
// new one, and an existing mapping keeps the old inode alive. On any failure
// the temporary is removed and the destination is left as it was.
//
// What this does not do: make concurrent writers agree. The last rename wins.
// For identical content (the same block compiled twice) that is harmless; for
// metadata.txt it means one process's entries can replace another's.

#pragma once

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <system_error>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace naab {
namespace runtime {

namespace atomic_file_detail {

// Unique by construction, not by luck: the pid separates processes and the
// counter separates calls within one. A seeded PRNG alone is not enough -- its
// state is copied by fork(), so two children of one parent generated the same
// names (the forked writers in tests/unit/inline_code_cache_test.cpp collided
// in 13 of 20 runs). The random part only guards against a recycled pid
// meeting a temporary left behind by a crash.
inline std::filesystem::path uniqueSibling(const std::filesystem::path& dest) {
    static std::atomic<unsigned long> counter{0};
#ifdef _WIN32
    const long pid = _getpid();
#else
    const long pid = static_cast<long>(getpid());
#endif
    char suffix[64];
    std::snprintf(suffix, sizeof(suffix), "%ld-%lu-%08x", pid, counter.fetch_add(1),
                  static_cast<unsigned>(std::random_device{}()));
    // ".tmp-" keeps the extension off ".so"/".cpp", so nothing that scans the
    // cache directory by extension mistakes a temporary for an entry.
    return dest.parent_path() / (dest.filename().string() + ".tmp-" + suffix);
}

inline void removeQuietly(const std::filesystem::path& p) {
    std::error_code ignored;
    std::filesystem::remove(p, ignored);
}

// Renames our finished temporary over dest; on failure removes it.
inline bool commit(const std::filesystem::path& tmp, const std::filesystem::path& dest,
                   std::error_code& ec) {
    std::filesystem::rename(tmp, dest, ec);
    if (ec) {
        removeQuietly(tmp);
        return false;
    }
    return true;
}

} // namespace atomic_file_detail

// Copies src over dest so that dest is never observed partially written.
inline bool copyFileAtomically(const std::filesystem::path& src,
                               const std::filesystem::path& dest,
                               std::error_code& ec) {
    ec.clear();
    auto tmp = atomic_file_detail::uniqueSibling(dest);
    // copy_options::none creates the temporary exclusively (O_EXCL), so a
    // name that already exists -- a symlink included -- fails the copy. That
    // file is not ours to remove; a copy that failed part-way is.
    if (!std::filesystem::copy_file(src, tmp, std::filesystem::copy_options::none, ec)) {
        if (ec != std::errc::file_exists) atomic_file_detail::removeQuietly(tmp);
        if (!ec) ec = std::make_error_code(std::errc::io_error);
        return false;
    }
    return atomic_file_detail::commit(tmp, dest, ec);
}

// Writes content to dest so that dest is never observed partially written.
inline bool writeFileAtomically(const std::filesystem::path& dest,
                                const std::string& content,
                                std::error_code& ec) {
    ec.clear();
    auto tmp = atomic_file_detail::uniqueSibling(dest);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << content;
        out.close();
        if (!out) {
            atomic_file_detail::removeQuietly(tmp);
            ec = std::make_error_code(std::errc::io_error);
            return false;
        }
    }
    return atomic_file_detail::commit(tmp, dest, ec);
}

} // namespace runtime
} // namespace naab
