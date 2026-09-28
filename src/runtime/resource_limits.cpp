#include "naab/resource_limits.h"
#include <cstdio>
#include <memory>
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
// Generation counter for cancellable timer threads: each arm bumps it and the
// timer fires only if its captured value still matches, so a stale timer from
// execution N cannot poison execution N+1 (R1). File-local to avoid a header
// ABI change.
//
// PER THREAD: a process-wide counter let one thread's clearTimeout() cancel
// another thread's timer -- a block run off the main thread removed the
// script's --timeout, and one REST request could cancel a concurrent
// request's. The timer thread holds a reference, so the counter outlives the
// thread that armed it.
static const std::shared_ptr<std::atomic<uint64_t>>& timerGeneration() {
    static thread_local const std::shared_ptr<std::atomic<uint64_t>> gen =
        std::make_shared<std::atomic<uint64_t>>(0);
    return gen;
}
#ifdef _WIN32
std::atomic<bool> ResourceLimiter::win_timer_cancel_{false};
#else
// V-RT-007: cancel flag for the POSIX timer thread (set by clearTimeout()).
std::atomic<bool> ResourceLimiter::posix_timer_cancel_{false};
#endif

// File-local to avoid a header ABI change (same reason as the generation counters).
static std::atomic<ResourceLimiter::TimeoutInterruptHook> g_timeout_interrupt_hook{nullptr};

void ResourceLimiter::setTimeoutInterruptHook(TimeoutInterruptHook hook) {
    g_timeout_interrupt_hook.store(hook, std::memory_order_release);
}

static void fireTimeoutInterruptHook(unsigned long long thread_key, bool script_wide) {
    if (auto hook = g_timeout_interrupt_hook.load(std::memory_order_acquire)) {
        hook(thread_key, script_wide);
    }
}

// This thread's own "my deadline fired" flag. Held by shared_ptr because the
// timer thread that sets it can outlive the thread that armed it.
static const std::shared_ptr<std::atomic<bool>>& threadFiredFlag() {
    static thread_local const std::shared_ptr<std::atomic<bool>> fired =
        std::make_shared<std::atomic<bool>>(false);
    return fired;
}

bool ResourceLimiter::threadTimerFired() {
    return threadFiredFlag()->load(std::memory_order_relaxed);
}

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

// Deadline of the innermost active timeout on this thread. ScopedTimeout
// reads it to nest: see resource_limits.h.
static thread_local bool t_has_deadline = false;
static thread_local bool t_deadline_script_wide = false;
static thread_local std::chrono::steady_clock::time_point t_deadline;

bool ResourceLimiter::currentDeadline(std::chrono::steady_clock::time_point& out) {
    if (t_has_deadline) out = t_deadline;
    return t_has_deadline;
}

bool ResourceLimiter::currentDeadline(std::chrono::steady_clock::time_point& out,
                                      bool& script_wide) {
    if (t_has_deadline) {
        out = t_deadline;
        script_wide = t_deadline_script_wide;
    }
    return t_has_deadline;
}

void ResourceLimiter::setExecutionTimeout(unsigned int seconds) {
    setDeadline(std::chrono::steady_clock::now() + std::chrono::seconds(seconds));
}

void ResourceLimiter::setDeadline(std::chrono::steady_clock::time_point deadline,
                                  bool script_wide) {
    if (!initialized_) {
        installSignalHandlers();
    }
    t_has_deadline = true;
    t_deadline = deadline;
    t_deadline_script_wide = script_wide;

    // V-ASYNC-001: reset the flags at the start of each new execution budget.
    // The process-wide flag belongs to the script-wide deadline only: a local
    // (nested or per-task) deadline must not reset it, or a pool worker arming
    // its own budget would erase the script's timeout after it fired.
    if (script_wide) global_shutdown_.store(false, std::memory_order_relaxed);
    timeout_triggered_ = false;
    auto fired = threadFiredFlag();
    fired->store(false, std::memory_order_relaxed);

#ifndef _WIN32
    // V-RT-007: capture the calling thread's id by value so the timer thread
    // can call pthread_kill() on the exact thread, not a random one in the pool.
    // tid is NOT stored as a static — it lives in the lambda closure so each
    // concurrent call to setDeadline() has its own independent timer.
    pthread_t tid = pthread_self();
    unsigned long long key = static_cast<unsigned long long>(tid);
    posix_timer_cancel_.store(false, std::memory_order_relaxed);
    auto gen = timerGeneration();
    uint64_t my_gen = ++*gen;
    std::thread([deadline, tid, key, gen, my_gen, fired, script_wide]() {
        using clock = std::chrono::steady_clock;
        while (clock::now() < deadline) {
            if (gen->load(std::memory_order_relaxed) != my_gen) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (gen->load(std::memory_order_relaxed) == my_gen) {
            // Set the flags first so isTimeoutTriggered() returns true even if
            // the signal is not delivered immediately (e.g. tight loops on
            // Android/Termux where SIGALRM may stay pending).
            fired->store(true, std::memory_order_relaxed);
            if (script_wide) {
                ResourceLimiter::global_shutdown_.store(true, std::memory_order_relaxed);
            }
            pthread_kill(tid, SIGALRM);
            fireTimeoutInterruptHook(key, script_wide);
        }
    }).detach();
    alarm(0);  // cancel any prior system-level alarm
#else
    // Windows has no alarm(). Spawn a detached timer thread that sets
    // global_shutdown_ at the deadline.
    //
    // R1 fix: generation counter prevents a stale timer from execution N
    // from poisoning execution N+1. Each new setDeadline() bumps the
    // counter; the timer thread captures the pre-bump value; before setting
    // global_shutdown_ it verifies the counter still matches. clearTimeout()
    // also bumps, so normal completion invalidates the in-flight timer.
    auto gen = timerGeneration();
    uint64_t my_gen = ++*gen;
    win_timer_cancel_.store(false, std::memory_order_relaxed);  // kept for compat
    std::thread([deadline, gen, my_gen, fired, script_wide]() {
        using clock = std::chrono::steady_clock;
        while (clock::now() < deadline) {
            if (gen->load(std::memory_order_relaxed) != my_gen) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (gen->load(std::memory_order_relaxed) == my_gen) {
            fired->store(true, std::memory_order_relaxed);
            if (script_wide) {
                ResourceLimiter::global_shutdown_.store(true, std::memory_order_relaxed);
            }
            fireTimeoutInterruptHook(0, script_wide);
        }
    }).detach();
#endif
}

void ResourceLimiter::clearTimeout() {
#ifndef _WIN32
    // V-RT-007: cancel the posix timer thread and any residual system alarm.
    // Bump generation counter to invalidate any in-flight timer thread
    // (matches Windows pattern — stale timer sees mismatched generation and exits).
    ++*timerGeneration();
    posix_timer_cancel_.store(true, std::memory_order_relaxed);
    alarm(0);
#else
    // R1 fix: bumping the generation counter invalidates any in-flight timer
    // thread — when it wakes, its captured my_gen no longer matches and it
    // returns without touching global_shutdown_.
    ++*timerGeneration();
    win_timer_cancel_.store(true, std::memory_order_relaxed);  // kept for compat
#endif
    timeout_triggered_ = false;
    threadFiredFlag()->store(false, std::memory_order_relaxed);
    // V-ASYNC-001: only the script-wide deadline owns the process-wide flag.
    // Clearing a LOCAL deadline must leave it alone -- a pool worker finishing
    // its own task would otherwise erase the script's timeout.
    if (!t_has_deadline || t_deadline_script_wide) {
        global_shutdown_.store(false, std::memory_order_relaxed);
    }
    t_has_deadline = false;
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
