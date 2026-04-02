#pragma once
//
// NAAb Platform Abstraction Layer (Phase 7.3 — Windows Portability)
//
// Provides a thin POSIX-to-Win32 bridge so higher-level code can call
// platform-neutral wrappers.  On Linux/macOS these forward directly to
// the POSIX APIs.  On Windows they delegate to Win32 equivalents.
//
// Usage:
//   #include "naab/platform.h"
//   naab::platform::setenv("FOO", "bar");
//   std::string val = naab::platform::getenv("FOO");
//   naab::platform::sleep_ms(50);
//

#include <string>
#include <cstdint>
#include <optional>

namespace naab {
namespace platform {

// ============================================================================
// Environment variables
// ============================================================================

// Get environment variable.  Returns nullopt if not set.
std::optional<std::string> getenv(const std::string& name);

// Set environment variable.  Returns true on success.
bool setenv(const std::string& name, const std::string& value, bool overwrite = true);

// Unset environment variable.  Returns true on success.
bool unsetenv(const std::string& name);

// ============================================================================
// Process
// ============================================================================

// Get current process ID.
int getpid();

// Sleep for the given number of milliseconds.
void sleep_ms(uint32_t ms);

// ============================================================================
// Filesystem helpers
// ============================================================================

// Resolve a symlink / canonical path.  Returns empty string on error.
std::string realpath(const std::string& path);

// Get the path of the current executable.  Returns empty string on error.
std::string executablePath();

// Path separator character ('/' on POSIX, '\\' on Windows).
char pathSeparator();

// ============================================================================
// Terminal / ANSI
// ============================================================================

// Returns true if the given file descriptor is an interactive terminal.
bool isatty(int fd);

// Enable ANSI escape processing on Windows console (no-op on POSIX).
void enableAnsiConsole();

// ============================================================================
// Atomic file write
// ============================================================================

// Write content to path atomically (write to temp, rename).
// Returns true on success.  Throws std::runtime_error on failure.
bool atomicWrite(const std::string& path, const std::string& content);

} // namespace platform
} // namespace naab
