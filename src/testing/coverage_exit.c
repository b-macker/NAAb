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
 */
#include <unistd.h>

extern void __gcov_dump(void);
extern void __real__exit(int status) __attribute__((noreturn));
extern void __real__Exit(int status) __attribute__((noreturn));

static pid_t naab_coverage_pid;

__attribute__((constructor)) static void naab_coverage_record_pid(void) {
    naab_coverage_pid = getpid();
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
