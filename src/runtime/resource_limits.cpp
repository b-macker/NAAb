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
thread_local volatile bool ResourceLimiter::timeout_triggered_ = false;
// V-ASYNC-001: process-wide shutdown flag. Set by signal handlers alongside
// timeout_triggered_. Unlike the thread_local flag, this is visible to ALL
// threads — including ThreadPool workers that never receive SIGALRM directly.
// Cleared by setExecutionTimeout() (new request) and clearTimeout() (RAII cleanup).
std::atomic<bool> ResourceLimiter::global_shutdown_{false};
#ifdef _WIN32
std::atomic<bool> ResourceLimiter::win_timer_cancel_{false};
// R1 fix: generation counter for cancellable Windows timer threads.
// File-local to avoid a header ABI change.
static std::atomic<uint64_t> g_win_timer_generation{0};
#else
// V-RT-007: cancel flag for the POSIX timer thread (set by clearTimeout()).
std::atomic<bool> ResourceLimiter::posix_timer_cancel_{false};
// Generation counter for cancellable POSIX timer threads (matches Windows pattern).
static std::atomic<uint64_t> g_posix_timer_generation{0};
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
    uint64_t my_gen = ++g_posix_timer_generation;
    std::thread([seconds, tid, my_gen]() {
        using clock = std::chrono::steady_clock;
        auto deadline = clock::now() + std::chrono::seconds(seconds);
        while (clock::now() < deadline) {
            if (g_posix_timer_generation.load(std::memory_order_relaxed) != my_gen) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (g_posix_timer_generation.load(std::memory_order_relaxed) == my_gen) {
            // Set global_shutdown_ first so isTimeoutTriggered() returns true
            // even if the signal is not delivered immediately (e.g. tight loops
            // on Android/Termux where SIGALRM may stay pending).
            ResourceLimiter::global_shutdown_.store(true, std::memory_order_relaxed);
            pthread_kill(tid, SIGALRM);
        }
    }).detach();
    alarm(0);  // cancel any prior system-level alarm
#else
    // Windows has no alarm(). Spawn a detached timer thread that sets
    // global_shutdown_ after the deadline.
    //
    // R1 fix: generation counter prevents a stale timer from execution N
    // from poisoning execution N+1. Each new setExecutionTimeout() bumps the
    // counter; the timer thread captures the pre-bump value; before setting
    // global_shutdown_ it verifies the counter still matches. clearTimeout()
    // also bumps, so normal completion invalidates the in-flight timer.
    uint64_t my_gen = ++g_win_timer_generation;
    win_timer_cancel_.store(false, std::memory_order_relaxed);  // kept for compat
    std::thread([seconds, my_gen]() {
        using clock = std::chrono::steady_clock;
        auto deadline = clock::now() + std::chrono::seconds(seconds);
        while (clock::now() < deadline) {
            if (g_win_timer_generation.load(std::memory_order_relaxed) != my_gen) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (g_win_timer_generation.load(std::memory_order_relaxed) == my_gen) {
            ResourceLimiter::global_shutdown_.store(true, std::memory_order_relaxed);
        }
    }).detach();
#endif
}

void ResourceLimiter::clearTimeout() {
#ifndef _WIN32
    // V-RT-007: cancel the posix timer thread and any residual system alarm.
    // Bump generation counter to invalidate any in-flight timer thread
    // (matches Windows pattern — stale timer sees mismatched generation and exits).
    ++g_posix_timer_generation;
    posix_timer_cancel_.store(true, std::memory_order_relaxed);
    alarm(0);
#else
    // R1 fix: bumping the generation counter invalidates any in-flight timer
    // thread — when it wakes, its captured my_gen no longer matches and it
    // returns without touching global_shutdown_.
    ++g_win_timer_generation;
    win_timer_cancel_.store(true, std::memory_order_relaxed);  // kept for compat
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
