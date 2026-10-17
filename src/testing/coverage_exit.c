/* coverage_exit.c -- linked ONLY into a NAAB_COVERAGE=ON build (CMakeLists.txt).
 *
 * gcov writes a process's counters from an exit handler, and _exit() skips exit
 * handlers. NAAb ends every HARD governance block with _exit(3) (src/cli/main.cpp,
 * 67 call sites when this was written), so without this every refusal path -- the
 * code a coverage report most needs to show -- would read as never executed.
 * The link wraps _exit/_Exit (-Wl,--wrap) to dump the counters first.
 *
 * Only in the process that started: a forked child calling _exit(127) after a
 * failed exec runs in a copy of a multithreaded process, where gcov's file I/O
 * could block on a lock another thread held at fork. Its few lines between fork
 * and exec are not recorded; that is the documented cost.
 *
 * SIGTERM and SIGINT likewise end a process without its exit handlers. A server
 * a test stops with `kill` -- `naab-lang api` in test_rest_hard_block_survives.sh
 * -- wrote nothing, so every REST function read "executed by no unit" though a
 * passing suite drives them (observed in the first full coverage run). While a
 * signal still has its default action at start-up, a handler dumps the counters,
 * restores the default and re-raises, so the process dies of the same signal with
 * the same status; an ignored or already-handled signal is left alone. __gcov_dump
 * is not async-signal-safe: a signal landing inside a stdio or malloc lock could
 * deadlock the handler, which a coverage run shows as a TIMEOUT, never as a
 * wrong reading.
 */
#include <signal.h>
#include <unistd.h>

extern void __gcov_dump(void);
extern void __real__exit(int status) __attribute__((noreturn));
extern void __real__Exit(int status) __attribute__((noreturn));

static pid_t naab_coverage_pid;

static void naab_coverage_on_signal(int sig) {
    if (getpid() == naab_coverage_pid) {
        __gcov_dump();
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static void naab_coverage_catch(int sig) {
    struct sigaction old;
    if (sigaction(sig, NULL, &old) == 0 && !(old.sa_flags & SA_SIGINFO) && old.sa_handler == SIG_DFL) {
        struct sigaction sa;
        sa.sa_handler = naab_coverage_on_signal;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESETHAND;
        sigaction(sig, &sa, NULL);
    }
}

// priority 101: before every default-priority constructor and C++ static initializer,
// so the handlers exist before any of them can run long or be signalled
__attribute__((constructor(101))) static void naab_coverage_record_pid(void) {
    naab_coverage_pid = getpid();
    naab_coverage_catch(SIGTERM);
    naab_coverage_catch(SIGINT);
}

void __wrap__exit(int status) {
    if (getpid() == naab_coverage_pid) {
        __gcov_dump();
    }
    __real__exit(status);
}

void __wrap__Exit(int status) {
    if (getpid() == naab_coverage_pid) {
        __gcov_dump();
    }
    __real__Exit(status);
}
