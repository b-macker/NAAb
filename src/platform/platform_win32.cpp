//
// NAAb Platform Abstraction — Win32 implementation (Phase 7.3 / Windows Sprint)
// Targets: Windows (MSVC and MinGW/UCRT64).
//
// Compiled only when _WIN32 is defined (see CMakeLists.txt platform selection).
//

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <io.h>       // _isatty
#include <stdlib.h>   // _fullpath
#include <direct.h>   // _getcwd

#include "naab/platform.h"
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace naab {
namespace platform {

// ============================================================================
// Environment variables
// ============================================================================

std::optional<std::string> getenv(const std::string& name) {
    // Use Win32 API so we get the process environment, not just the CRT copy.
    DWORD len = ::GetEnvironmentVariableA(name.c_str(), nullptr, 0);
    if (len == 0) return std::nullopt;   // Not found (or truly empty)
    std::string buf(len, '\0');
    ::GetEnvironmentVariableA(name.c_str(), &buf[0], len);
    // GetEnvironmentVariable includes the NUL in 'len' but writes it to buf[len-1].
    // The string constructor from len already has NUL at position len-1; trim it.
    if (!buf.empty() && buf.back() == '\0') buf.pop_back();
    return buf;
}

bool setenv(const std::string& name, const std::string& value, bool overwrite) {
    if (!overwrite) {
        // Check existence first
        DWORD exists = ::GetEnvironmentVariableA(name.c_str(), nullptr, 0);
        if (exists != 0) return true;  // Already set — skip
    }
    // _putenv_s also syncs the CRT 'environ' array used by getenv().
    return ::_putenv_s(name.c_str(), value.c_str()) == 0;
}

bool unsetenv(const std::string& name) {
    // Remove from both the Win32 env block and the CRT environ table.
    ::SetEnvironmentVariableA(name.c_str(), nullptr);
    return ::_putenv_s(name.c_str(), "") == 0;
}

// ============================================================================
// Process
// ============================================================================

int getpid() {
    return static_cast<int>(::GetCurrentProcessId());
}

void sleep_ms(uint32_t ms) {
    ::Sleep(static_cast<DWORD>(ms));
}

// ============================================================================
// Filesystem helpers
// ============================================================================

std::string realpath(const std::string& path) {
    char resolved[MAX_PATH];
    if (::_fullpath(resolved, path.c_str(), MAX_PATH) == nullptr) return "";
    return std::string(resolved);
}

std::string executablePath() {
    char buf[MAX_PATH];
    DWORD len = ::GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return "";
    return std::string(buf, len);
}

char pathSeparator() {
    return '\\';
}

// ============================================================================
// Terminal / ANSI
// ============================================================================

bool isatty(int fd) {
    return ::_isatty(fd) != 0;
}

void enableAnsiConsole() {
    // Enable ANSI/VT100 escape processing on Windows 10+ consoles.
    // Also set UTF-8 code pages for correct Unicode output.
    ::SetConsoleCP(65001);
    ::SetConsoleOutputCP(65001);

    HANDLE hOut = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    if (!::GetConsoleMode(hOut, &mode)) return;
    ::SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    // Also apply to stderr
    HANDLE hErr = ::GetStdHandle(STD_ERROR_HANDLE);
    if (hErr != INVALID_HANDLE_VALUE) {
        DWORD errMode = 0;
        if (::GetConsoleMode(hErr, &errMode)) {
            ::SetConsoleMode(hErr, errMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }
}

// ============================================================================
// Atomic file write
// ============================================================================

bool atomicWrite(const std::string& path, const std::string& content) {
    std::string tmp_path = path + ".naab_tmp";

    {
        std::ofstream ofs(tmp_path, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!ofs.is_open()) {
            throw std::runtime_error("atomicWrite: cannot open temp file: " + tmp_path);
        }
        ofs << content;
        if (!ofs.good()) {
            throw std::runtime_error("atomicWrite: write error for: " + tmp_path);
        }
    }  // ofs flushed and closed here

    // MoveFileExA with MOVEFILE_REPLACE_EXISTING is the closest Win32 equivalent
    // to POSIX rename() — it is atomic within the same volume.
    if (!::MoveFileExA(tmp_path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        ::DeleteFileA(tmp_path.c_str());
        throw std::runtime_error(
            "atomicWrite: MoveFileEx failed for: " + path +
            " (error " + std::to_string(::GetLastError()) + ")");
    }

    return true;
}

} // namespace platform
} // namespace naab

#endif // _WIN32
