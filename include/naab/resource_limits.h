#pragma once

#include <atomic>
#include <chrono>
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

    // Arm the timer for an absolute deadline (setExecutionTimeout is this with
    // now + seconds). currentDeadline() reports this thread's active deadline,
    // if any; ScopedTimeout uses the pair to nest.
    //
    // `script_wide`: when this deadline fires, does the whole SCRIPT stop, or
    // only the thread that armed it? Only the outermost timeout of a run (the
    // CLI's or a REST request's) is script-wide. Every timeout used to be: a
    // firing set the process-wide flag and every arm or clear RESET it, so a
    // pool worker honouring a 50 ms async-executor timeout would have stopped
    // the whole script, and a worker merely arming one could erase the
    // script's own --timeout after it fired.
    static void setDeadline(std::chrono::steady_clock::time_point deadline,
                            bool script_wide = true);
    static bool currentDeadline(std::chrono::steady_clock::time_point& out);
    static bool currentDeadline(std::chrono::steady_clock::time_point& out,
                                bool& script_wide);

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

    // Called from the timer thread (never from a signal handler) right after
    // an execution timeout fires. An in-process runtime that cannot poll
    // isTimeoutTriggered() itself -- embedded CPython -- registers one to be
    // interrupted. QuickJS needs none: its interrupt handler polls the flag.
    //
    // `thread_key` identifies the thread whose deadline fired (pthread_self()
    // as an integer on POSIX, 0 elsewhere); `script_wide` says whether every
    // thread of the script is stopping or only that one. The hook may block:
    // it runs on the timer thread, which has nothing else to do.
    using TimeoutInterruptHook = void (*)(unsigned long long thread_key,
                                          bool script_wide);
    static void setTimeoutInterruptHook(TimeoutInterruptHook hook);

    // Check if timeout has been triggered on this thread OR process-wide.
    // thread_local flag: set when this specific thread received SIGALRM.
    // global_shutdown_: set by the signal handler; visible to ALL threads,
    // including async worker threads that never receive SIGALRM directly.
    // Together they cover both the main execution thread and ThreadPool workers.
    // Plus this thread's own timer (threadTimerFired), which is how a LOCAL
    // timeout reaches only the thread that armed it.
    static bool isTimeoutTriggered() {
        return timeout_triggered_ || global_shutdown_.load(std::memory_order_relaxed) ||
               threadTimerFired();
    }
    static bool threadTimerFired();

private:
    static void handleAlarm(int sig);
    static void handleCpuLimit(int sig);

    static bool initialized_;
    static thread_local volatile bool timeout_triggered_;
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

// RAII timeout that NESTS. The script's run is wrapped in one (CLI, REST),
// and executors wrap each block in another. There is one timer per thread, so
// these used to clobber each other: the block's scope re-armed the timer with
// its own budget and its destructor CLEARED it, so one <<javascript>>
// expression silently removed --timeout for the rest of the script (measured:
// a `while true` after it ran until killed from outside).
//
// Now a scope can only TIGHTEN the deadline in force, never extend it, and on
// exit restores the outer deadline instead of clearing it. `seconds == 0`
// means "no limit of its own" (the UNRESTRICTED sandbox preset); it used to arm
// a zero-second timer that killed every subprocess on start.
//
// Scope: the OUTERMOST seconds-based timeout on a thread is script-wide (the
// CLI and REST arm the run's deadline this way); a nested one, or any
// Local timeout, stops only the thread that armed it. Async executors use
// Local: their per-task budget runs on a pool thread with no outer deadline,
// and must not stop the script when it expires.
class ScopedTimeout {
public:
    enum class Scope { Auto, Local };

    explicit ScopedTimeout(unsigned int seconds)
        : ScopedTimeout(std::chrono::milliseconds(seconds * 1000ULL), Scope::Auto) {}

    ScopedTimeout(std::chrono::milliseconds budget, Scope scope) {
        had_outer_ = ResourceLimiter::currentDeadline(outer_, outer_script_wide_);
        if (budget.count() <= 0) return;
        auto mine = std::chrono::steady_clock::now() + budget;
        if (had_outer_ && outer_ <= mine) return;  // the outer limit is tighter
        bool script_wide = (scope == Scope::Auto) && !had_outer_;
        ResourceLimiter::setDeadline(mine, script_wide);
        armed_ = true;
    }

    ~ScopedTimeout() {
        if (!armed_) return;
        if (had_outer_) {
            // Re-arms for the remaining time, or fires at once if it has passed.
            ResourceLimiter::setDeadline(outer_, outer_script_wide_);
        } else {
            ResourceLimiter::clearTimeout();
        }
    }

    // Prevent copying
    ScopedTimeout(const ScopedTimeout&) = delete;
    ScopedTimeout& operator=(const ScopedTimeout&) = delete;

private:
    std::chrono::steady_clock::time_point outer_{};
    bool had_outer_ = false;
    bool outer_script_wide_ = false;
    bool armed_ = false;
};

} // namespace security
} // namespace naab
