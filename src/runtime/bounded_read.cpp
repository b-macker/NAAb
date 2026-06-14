// BoundedRead implementation — see include/naab/bounded_read.h
// V-RT-014 (R24): cap loader reads at 10 MB and reject symlinks.
// V-RT-015 (R25): atomic open — O_NOFOLLOW + fstat on same fd eliminates TOCTOU.

#include "naab/bounded_read.h"

#include <fstream>

#ifndef _WIN32
#  include <sys/stat.h>
#  include <unistd.h>
#  include <fcntl.h>
#endif

namespace naab {

std::optional<std::string> readFileBounded(const std::string& path,
                                           std::size_t max_bytes) {
#ifndef _WIN32
    // V-RT-015: atomic open — O_NOFOLLOW rejects symlinks at the kernel level,
    // and fstat on the returned fd verifies the same inode is a regular file.
    // This eliminates the lstat-then-ifstream TOCTOU race.
    int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return std::nullopt;
    struct FdGuard { int f; ~FdGuard() { if (f >= 0) ::close(f); } } guard{fd};

    struct stat st;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        return std::nullopt;
    }

    std::string content;
    content.reserve(4096); // most source files are small
    char buf[65536];
    ssize_t n;
    while ((n = ::read(fd, buf, sizeof(buf))) > 0) {
        auto chunk = static_cast<std::size_t>(n);
        if (content.size() + chunk > max_bytes) {
            content.append(buf, max_bytes - content.size());
            break; // silently truncate — partial scan/parse is better than OOM
        }
        content.append(buf, chunk);
    }
    if (n < 0) return std::nullopt;
    return content;
#else
    // Windows: no symlink TOCTOU concern — use ifstream fallback
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return std::nullopt;

    std::string content;
    content.reserve(4096);
    char buf[65536];
    while (file.read(buf, sizeof(buf)) || file.gcount() > 0) {
        auto chunk = static_cast<std::size_t>(file.gcount());
        if (content.size() + chunk > max_bytes) {
            content.append(buf, max_bytes - content.size());
            break;
        }
        content.append(buf, chunk);
    }
    return content;
#endif
}

} // namespace naab
