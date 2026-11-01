// The ThreadSanitizer job's positive control: a data race TSan must report.
// run_tsan.sh compiles it with the job's compiler and flags, runs it under the
// same TSAN_OPTIONS as every suite, and requires a report in the log -- so a
// quiet run of the suites means "no race seen", not "the logs were never
// written" (a broken probe reports a finding, not an error).
#include <cstdio>
#include <thread>

static int shared_counter = 0;  // written by two threads with no synchronisation

int main() {
    std::thread a([] { for (int i = 0; i < 100000; i++) shared_counter++; });
    std::thread b([] { for (int i = 0; i < 100000; i++) shared_counter++; });
    a.join();
    b.join();
    std::printf("%d\n", shared_counter > 0 ? 1 : 0);
    return 0;
}
