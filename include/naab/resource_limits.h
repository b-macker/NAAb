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

    // Request a cooperative shutdown — sets global_shutdown_ so the VM loop
    // and subprocess polling waits bail out at the next checkpoint. Used by
    // the Windows Ctrl-C handler and may be called from any thread.
    static void requestShutdown() {
        global_shutdown_.store(true, std::memory_order_relaxed);
    }

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
#ifdef _WIN32
    // Windows has no alarm(). setExecutionTimeout() spawns a timer thread
    // that sets global_shutdown_ after N seconds. This flag cancels it early
    // (set by clearTimeout() when the script finishes normally).
    static std::atomic<bool> win_timer_cancel_;
#else
    // V-RT-007: cancel flag for the POSIX timer thread. The target tid is
    // captured by value in the detached lambda — no static storage needed.
    static std::atomic<bool> posix_timer_cancel_;
#endif
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
