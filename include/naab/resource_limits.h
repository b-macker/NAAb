#pragma once

#include <atomic>
#include <cstddef>
#include <string>
#include <stdexcept>
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

    // Check if timeout has been triggered on this thread OR process-wide.
    // thread_local flag: set when this specific thread received SIGALRM.
    // global_shutdown_: set by the signal handler; visible to ALL threads,
    // including async worker threads that never receive SIGALRM directly.
    // Together they cover both the main execution thread and ThreadPool workers.
    static bool isTimeoutTriggered() {
        return timeout_triggered_ || global_shutdown_.load(std::memory_order_relaxed);
    }

private:
    static void handleAlarm(int sig);
    static void handleCpuLimit(int sig);

    static bool initialized_;
    static thread_local bool timeout_triggered_;
    // V-ASYNC-001: process-wide flag visible to all threads (async workers).
    // Set alongside timeout_triggered_ in signal handlers; cleared on each
    // new setExecutionTimeout() call and in clearTimeout().
    static std::atomic<bool> global_shutdown_;
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
