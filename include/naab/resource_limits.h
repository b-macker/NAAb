#pragma once

#include <cstddef>
#include <string>
#include <functional>

namespace naab {
namespace security {

// Exception thrown when resource limit is exceeded
class ResourceLimitException : public std::runtime_error {
public:
    explicit ResourceLimitException(const std::string& msg)
        : std::runtime_error(msg) {}
};

// Resource limiter for execution timeout and memory limits
class ResourceLimiter {
public:
    // Set execution timeout in seconds (uses alarm())
    // Throws ResourceLimitException when timeout expires
    static void setExecutionTimeout(unsigned int seconds);

    // Clear the current timeout
    static void clearTimeout();

    // Set memory limit in megabytes (uses setrlimit)
    static void setMemoryLimit(size_t megabytes);

    // Set CPU time limit in seconds (uses setrlimit)
    static void setCpuTimeLimit(unsigned int seconds);

    // Install signal handlers for SIGALRM and SIGXCPU
    static void installSignalHandlers();

    // Check if signal handlers are installed
    static bool isInitialized();

    // Disable all resource limits (for cleanup)
    static void disableAll();

private:
    static void handleAlarm(int sig);
    static void handleCpuLimit(int sig);

    static bool initialized_;
    static bool timeout_triggered_;
};

// RAII helper for automatic timeout cleanup
class ScopedTimeout {
public:
    explicit ScopedTimeout(unsigned int seconds) {
        ResourceLimiter::setExecutionTimeout(seconds);
    }

    ~ScopedTimeout() {
        ResourceLimiter::clearTimeout();
    }

    // Prevent copying
    ScopedTimeout(const ScopedTimeout&) = delete;
    ScopedTimeout& operator=(const ScopedTimeout&) = delete;
};

} // namespace security
} // namespace naab
