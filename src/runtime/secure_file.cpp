// SecureFile implementation — see include/naab/secure_file.h
//
// V-SC-005 (R24): refuse to follow symlinks when writing security-sensitive
// files (lockfile, signatures). POSIX uses O_NOFOLLOW; Windows rejects reparse
// points via FILE_FLAG_OPEN_REPARSE_POINT + GetFileInformationByHandle.

#include "naab/secure_file.h"

#ifndef _WIN32
#  include <fcntl.h>
#  include <unistd.h>
#  include <sys/stat.h>
#  include <cerrno>
#else
#  include <windows.h>
#endif

namespace naab {
namespace security {

#ifndef _WIN32

bool writeFileSecure(const std::string& path, const std::string& content) {
    // O_TRUNC replaces existing regular file contents.
    // O_NOFOLLOW makes open() fail with ELOOP if `path` is a symlink.
    // O_CLOEXEC prevents the fd from leaking across fork/exec.
    int fd = ::open(path.c_str(),
                    O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC,
                    0644);
    if (fd < 0) return false;

    // Defense-in-depth: confirm the fd refers to a regular file, not e.g.
    // a FIFO or device that was swapped in via TOCTOU.
    struct stat st;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        return false;
    }

    const char* data = content.data();
    size_t remaining = content.size();
    while (remaining > 0) {
        ssize_t n = ::write(fd, data, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return false;
        }
        data += n;
        remaining -= static_cast<size_t>(n);
    }
    return ::close(fd) == 0;
}

#else // _WIN32

bool writeFileSecure(const std::string& path, const std::string& content) {
    // FILE_FLAG_OPEN_REPARSE_POINT: open the reparse point itself rather than
    // following it; combined with the reparse-point check below, this rejects
    // symlinks/junctions.
    HANDLE h = CreateFileA(path.c_str(),
                           GENERIC_WRITE,
                           0, // no sharing
                           nullptr,
                           CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(h, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        CloseHandle(h);
        DeleteFileA(path.c_str());
        return false;
    }

    const char* data = content.data();
    size_t remaining = content.size();
    while (remaining > 0) {
        DWORD chunk = remaining > 0x10000000 ? 0x10000000 : static_cast<DWORD>(remaining);
        DWORD written = 0;
        if (!WriteFile(h, data, chunk, &written, nullptr) || written == 0) {
            CloseHandle(h);
            return false;
        }
        data += written;
        remaining -= written;
    }
    CloseHandle(h);
    return true;
}

#endif

} // namespace security
} // namespace naab
