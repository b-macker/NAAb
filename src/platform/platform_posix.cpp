//
// NAAb Platform Abstraction — POSIX implementation (Phase 7.3)
// Targets: Linux, macOS, Android/Termux, other POSIX-compliant systems.
//
// Windows implementation lives in platform_win32.cpp (not yet written).
// The CMakeLists.txt selects the correct source based on the platform.
//

#include "naab/platform.h"

#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <chrono>

#include <unistd.h>
#include <fcntl.h>
#include <limits.h>

#ifdef __APPLE__
#  include <mach-o/dyld.h>
#endif

namespace naab {
namespace platform {

// ============================================================================
// Environment variables
// ============================================================================

std::optional<std::string> getenv(const std::string& name) {
    const char* val = ::getenv(name.c_str());
    if (!val) return std::nullopt;
    return std::string(val);
}

bool setenv(const std::string& name, const std::string& value, bool overwrite) {
    return ::setenv(name.c_str(), value.c_str(), overwrite ? 1 : 0) == 0;
}

bool unsetenv(const std::string& name) {
    return ::unsetenv(name.c_str()) == 0;
}

// ============================================================================
// Process
// ============================================================================

int getpid() {
    return static_cast<int>(::getpid());
}

void sleep_ms(uint32_t ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// ============================================================================
// Filesystem helpers
// ============================================================================

std::string realpath(const std::string& path) {
    char resolved[PATH_MAX];
    if (::realpath(path.c_str(), resolved) == nullptr) return "";
    return std::string(resolved);
}

std::string executablePath() {
#if defined(__linux__) || defined(__ANDROID__)
    char buf[PATH_MAX];
    ssize_t len = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        return std::string(buf);
    }
    return "";
#elif defined(__APPLE__)
    char buf[PATH_MAX];
    uint32_t size = static_cast<uint32_t>(sizeof(buf));
    if (_NSGetExecutablePath(buf, &size) == 0) {
        char resolved[PATH_MAX];
        if (::realpath(buf, resolved) != nullptr) return std::string(resolved);
        return std::string(buf);
    }
    return "";
#else
    // Fallback: try argv[0] resolution (not reliable, but better than empty)
    return "";
#endif
}

char pathSeparator() {
    return '/';
}

// ============================================================================
// Terminal / ANSI
// ============================================================================

bool isatty(int fd) {
    return ::isatty(fd) != 0;
}

void enableAnsiConsole() {
    // No-op on POSIX — ANSI escape codes are natively supported.
}

// ============================================================================
// Atomic file write
// ============================================================================

bool atomicWrite(const std::string& path, const std::string& content) {
    // Write to a temp file alongside the target, then rename atomically.
    std::string tmp_path = path + ".naab_tmp";

    {
        // Create temp file with restricted permissions (0600) before writing
        int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) {
            throw std::runtime_error("atomicWrite: cannot create temp file: " + tmp_path);
        }
        ::close(fd);
        std::ofstream ofs(tmp_path, std::ios::out | std::ios::trunc);
        if (!ofs.is_open()) {
            throw std::runtime_error(
                "atomicWrite: cannot open temp file: " + tmp_path);
        }
        ofs << content;
        if (!ofs.good()) {
            throw std::runtime_error(
                "atomicWrite: write error for: " + tmp_path);
        }
    }  // ofs closed and flushed here

    // rename() is atomic on POSIX within the same filesystem
    if (::rename(tmp_path.c_str(), path.c_str()) != 0) {
        ::unlink(tmp_path.c_str());
        throw std::runtime_error(
            "atomicWrite: rename failed for: " + path +
            " (" + ::strerror(errno) + ")");
    }

    return true;
}

} // namespace platform
} // namespace naab
