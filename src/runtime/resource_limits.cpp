#include "naab/resource_limits.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>

#ifndef _WIN32
#  include <csignal>
#  include <unistd.h>
#  include <sys/resource.h>
#  include <cerrno>
#  include <pthread.h>
#  include <thread>
#  include <chrono>
#else
#  include <thread>
#  include <chrono>
#endif

namespace naab {
namespace security {

// Static member initialization
bool ResourceLimiter::initialized_ = false;
// thread_local: each thread (i.e. each REST API request) has its own flag.
// A timeout on request N cannot contaminate request N+1 running concurrently.
// NOTE: setExecutionTimeout() must be called from the thread that will execute
// the script — the SIGALRM signal is delivered to whichever thread handles it,
// which on Linux is typically the signalling thread or the process's main thread.
// For multi-tenant use, prefer per-thread alarm delivery via timer_create(CLOCK_THREAD_CPUTIME_ID).
thread_local bool ResourceLimiter::timeout_triggered_ = false;
// V-ASYNC-001: process-wide shutdown flag. Set by signal handlers alongside
// timeout_triggered_. Unlike the thread_local flag, this is visible to ALL
// threads — including ThreadPool workers that never receive SIGALRM directly.
// Cleared by setExecutionTimeout() (new request) and clearTimeout() (RAII cleanup).
std::atomic<bool> ResourceLimiter::global_shutdown_{false};
#ifdef _WIN32
std::atomic<bool> ResourceLimiter::win_timer_cancel_{false};
#else
// V-RT-007: cancel flag for the POSIX timer thread (set by clearTimeout()).
std::atomic<bool> ResourceLimiter::posix_timer_cancel_{false};
#endif

void ResourceLimiter::installSignalHandlers() {
    if (initialized_) {
        return;
    }

#ifndef _WIN32
    // Install SIGALRM handler for execution timeout
    struct sigaction sa_alarm;
    std::memset(&sa_alarm, 0, sizeof(sa_alarm));
    sa_alarm.sa_handler = handleAlarm;
    sa_alarm.sa_flags = SA_RESTART;  // Restart interrupted system calls
    sigemptyset(&sa_alarm.sa_mask);

    if (sigaction(SIGALRM, &sa_alarm, nullptr) != 0) {
        throw std::runtime_error("Failed to install SIGALRM handler");
    }

    // Install SIGXCPU handler for CPU time limit
    struct sigaction sa_cpu;
    std::memset(&sa_cpu, 0, sizeof(sa_cpu));
    sa_cpu.sa_handler = handleCpuLimit;
    sa_cpu.sa_flags = SA_RESTART;
    sigemptyset(&sa_cpu.sa_mask);

    if (sigaction(SIGXCPU, &sa_cpu, nullptr) != 0) {
        throw std::runtime_error("Failed to install SIGXCPU handler");
    }
#endif

    initialized_ = true;
}

bool ResourceLimiter::isInitialized() {
    return initialized_;
}

void ResourceLimiter::setExecutionTimeout(unsigned int seconds) {
    if (!initialized_) {
        installSignalHandlers();
    }

    // V-ASYNC-001: reset both flags at the start of each new execution budget.
    global_shutdown_.store(false, std::memory_order_relaxed);
    timeout_triggered_ = false;

#ifndef _WIN32
    // V-RT-007: capture the calling thread's id by value so the timer thread
    // can call pthread_kill() on the exact thread, not a random one in the pool.
    // tid is NOT stored as a static — it lives in the lambda closure so each
    // concurrent call to setExecutionTimeout() has its own independent timer.
    pthread_t tid = pthread_self();
    posix_timer_cancel_.store(false, std::memory_order_relaxed);
    std::thread([seconds, tid]() {
        using clock = std::chrono::steady_clock;
        auto deadline = clock::now() + std::chrono::seconds(seconds);
        while (clock::now() < deadline) {
            if (ResourceLimiter::posix_timer_cancel_.load(std::memory_order_relaxed)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!ResourceLimiter::posix_timer_cancel_.load(std::memory_order_relaxed)) {
            pthread_kill(tid, SIGALRM);
        }
    }).detach();
    alarm(0);  // cancel any prior system-level alarm
#else
    // Windows has no alarm(). Spawn a detached timer thread that polls
    // win_timer_cancel_ every 50ms and sets global_shutdown_ at the deadline.
    // global_shutdown_ is then picked up by isTimeoutTriggered() in VM_NEXT().
    win_timer_cancel_.store(false, std::memory_order_relaxed);
    std::thread([seconds]() {
        using clock = std::chrono::steady_clock;
        auto deadline = clock::now() + std::chrono::seconds(seconds);
        while (clock::now() < deadline) {
            if (ResourceLimiter::win_timer_cancel_.load(std::memory_order_relaxed)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        // V-ASYNC-001r (Windows): timer thread cannot set a thread_local variable of
        // the execution thread, so we still use global_shutdown_ here. For single-tenant
        // Windows builds this is acceptable; multi-tenant Windows would need a per-execution
        // cancellation token (future work). The POSIX path uses thread_local only.
        if (!ResourceLimiter::win_timer_cancel_.load(std::memory_order_relaxed)) {
            ResourceLimiter::global_shutdown_.store(true, std::memory_order_relaxed);
        }
    }).detach();
#endif
}

void ResourceLimiter::clearTimeout() {
#ifndef _WIN32
    // V-RT-007: cancel the posix timer thread and any residual system alarm.
    posix_timer_cancel_.store(true, std::memory_order_relaxed);
    alarm(0);
#else
    // Signal the Windows timer thread to exit without setting global_shutdown_.
    // Must be set BEFORE resetting global_shutdown_ to close the race window.
    win_timer_cancel_.store(true, std::memory_order_relaxed);
#endif
    timeout_triggered_ = false;
    global_shutdown_.store(false, std::memory_order_relaxed);  // V-ASYNC-001
}

void ResourceLimiter::setMemoryLimit(size_t megabytes) {
    // WARNING: This sets RLIMIT_AS which is PROCESS-WIDE and persists after
    // this call returns. It affects ALL subsequent fork/exec/system calls
    // because child processes inherit the limit and cannot allocate memory
    // for the dynamic linker if the limit is too restrictive.
    //
    // PREFER language-native memory limits instead:
    //   - QuickJS: JS_SetMemoryLimit(rt, bytes)
    //   - Python: resource.setrlimit() within the Python process
    //   - C++/Rust/C#: Compile-time or subprocess-level limits
    //
    // If you MUST use this, call disableAll() afterwards to clear it.
#ifndef _WIN32
    fprintf(stderr,
        "[WARNING] ResourceLimiter::setMemoryLimit(%zu MB) sets process-wide RLIMIT_AS.\n"
        "  This will break ALL subsequent fork/exec/system calls.\n"
        "  Use language-native memory limits instead (e.g., JS_SetMemoryLimit for QuickJS).\n"
        "  Call ResourceLimiter::disableAll() to clear after use.\n",
        megabytes);

    struct rlimit limit;
    limit.rlim_cur = megabytes * 1024 * 1024;  // Convert MB to bytes
    limit.rlim_max = megabytes * 1024 * 1024;

    if (setrlimit(RLIMIT_AS, &limit) != 0) {
        throw std::runtime_error("Failed to set memory limit: " + std::string(std::strerror(errno)));
    }
#else
    // On Windows, use Job Objects for per-child memory limits (not yet implemented).
    // Process-wide RLIMIT_AS has no direct equivalent.
    fprintf(stderr,
        "[INFO] ResourceLimiter::setMemoryLimit(%zu MB): memory limits not enforced on Windows.\n"
        "  Use language-native limits or Job Objects for subprocess enforcement.\n",
        megabytes);
    (void)megabytes;
#endif
}

void ResourceLimiter::setCpuTimeLimit(unsigned int seconds) {
    if (!initialized_) {
        installSignalHandlers();
    }

#ifndef _WIN32
    struct rlimit limit;
    limit.rlim_cur = seconds;
    limit.rlim_max = seconds;

    if (setrlimit(RLIMIT_CPU, &limit) != 0) {
        throw std::runtime_error("Failed to set CPU time limit: " + std::string(std::strerror(errno)));
    }
#else
    // On Windows, CPU time limits require Job Objects (not yet implemented).
    (void)seconds;
#endif
}

void ResourceLimiter::disableAll() {
    clearTimeout();

#ifndef _WIN32
    // Remove memory limit (set to maximum)
    struct rlimit limit;
    limit.rlim_cur = RLIM_INFINITY;
    limit.rlim_max = RLIM_INFINITY;

    setrlimit(RLIMIT_AS, &limit);
    setrlimit(RLIMIT_CPU, &limit);
#endif
}

void ResourceLimiter::handleAlarm(int sig) {
    (void)sig;  // Unused parameter
    // V-ASYNC-001r: set only the thread-local flag, NOT global_shutdown_.
    // global_shutdown_ is process-wide — setting it here would terminate every concurrent
    // script in the process (multi-tenant contamination). timeout_triggered_ is thread_local
    // so it only affects the thread whose alarm fired.
    timeout_triggered_ = true;

    // Note: We can't throw exceptions from signal handlers
    // The timeout will be detected when control returns to normal code
}

void ResourceLimiter::handleCpuLimit(int sig) {
    (void)sig;  // Unused parameter
    // V-ASYNC-001r: same fix — thread-local flag only, not global_shutdown_.
    timeout_triggered_ = true;
}

} // namespace security
} // namespace naab
