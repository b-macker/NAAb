// BoundedRead implementation — see include/naab/bounded_read.h
// V-RT-014 (R24): cap loader reads at 10 MB and reject symlinks.

#include "naab/bounded_read.h"

#include <fstream>

#ifndef _WIN32
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace naab {

std::optional<std::string> readFileBounded(const std::string& path,
                                           std::size_t max_bytes) {
#ifndef _WIN32
    // Fast reject: if the path itself is a symlink, refuse the read.
    struct stat lst;
    if (::lstat(path.c_str(), &lst) != 0) return std::nullopt;
    if (S_ISLNK(lst.st_mode)) return std::nullopt;
    if (!S_ISREG(lst.st_mode)) return std::nullopt;
#endif

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return std::nullopt;

    std::string content;
    content.reserve(4096); // most source files are small
    char buf[65536];
    while (file.read(buf, sizeof(buf)) || file.gcount() > 0) {
        auto chunk = static_cast<std::size_t>(file.gcount());
        if (content.size() + chunk > max_bytes) {
            content.append(buf, max_bytes - content.size());
            break; // silently truncate — partial scan/parse is better than OOM
        }
        content.append(buf, chunk);
    }
    return content;
}

} // namespace naab
