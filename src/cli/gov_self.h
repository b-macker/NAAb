// What naab-gov's commands share when they run ANOTHER naab-gov command as a
// child of the same binary (explain, patch-check, heal): one copy, so a fix to
// how the child is started cannot reach one command and miss the others.
#pragma once

#include "naab/subprocess_helpers.h"

#include <fcntl.h>
#include <filesystem>
#include <string>
#include <vector>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace naab::govcli {

// The running binary's own path: never looked up by name on PATH.
inline std::string selfPath(const char* argv0) {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return std::string(buf, n);
#else
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec && !p.empty()) return p.string();
#endif
    return argv0 ? std::string(argv0) : std::string("naab-gov");
}

// Runs `self args...` with the null device as its stdin (A122: an engine's own
// child never reads the caller's input) and returns its exit status.
inline int runSelf(const std::string& self, const std::vector<std::string>& args,
                   std::string& out, std::string& err) {
    return naab::runtime::execute_subprocess_with_pipes(
        self, args, out, err, nullptr, nullptr, naab::runtime::ChildStdin::Null);
}

// Points this process's own stdin at the null device, before any config load.
inline void detachStdin() {
#ifdef _WIN32
    int fd = _open("NUL", _O_RDONLY);
    if (fd >= 0) { _dup2(fd, 0); if (fd != 0) _close(fd); }
#else
    int fd = ::open("/dev/null", O_RDONLY);
    if (fd >= 0) { ::dup2(fd, 0); if (fd != 0) ::close(fd); }
#endif
}

}  // namespace naab::govcli
