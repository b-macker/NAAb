// Linked only into test_codemap_coverage.sh's fixture build (the index and
// profile suites compile fixture.cpp and other.cpp alone). Everything here runs
// from static initializers, so neither main() nor any line the other suites pin
// had to change.
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

// A function template instantiated WITH a lambda type. gcov names the
// instantiation `apply_fn<uses_apply()::{lambda()#1}>(...)` -- a lambda in its
// template and parameter lists, not a lambda itself -- and it must map to
// apply_fn.
template <typename F>
int apply_fn(F f) {
    return f() + 1;
}

int uses_apply() {
    return apply_fn([] { return 6; });
}

static int g_applied = uses_apply();

// A long-running process stopped by SIGTERM (a server a test `kill`s): with
// FIXTURE_READY_FILE set, write that file and wait for a signal.
static int wait_for_signal() {
    const char* ready = std::getenv("FIXTURE_READY_FILE");
    if (!ready) {
        return 0;
    }
    if (FILE* f = std::fopen(ready, "w")) {
        std::fclose(f);
    }
    for (;;) {
        pause();
    }
}

static int g_waited = wait_for_signal();
