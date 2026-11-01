#pragma once
// UTC calendar time from a time_t, safe on any thread.
//
// gmtime() returns a pointer to ONE static struct shared by every thread, so
// two threads formatting a timestamp at once overwrite each other's -- the
// audit logger and the tamper-evident logger did, from async fns and
// agent.batch workers, and ThreadSanitizer reported it (Bundle 9). Call this
// instead; tests/self-audit/test_reentrant_time.sh fails on a new gmtime(),
// localtime(), ctime() or asctime() call in src/ or include/.
#include <ctime>

namespace naab {

inline std::tm utcTime(std::time_t t) {
    std::tm out{};
#ifdef _WIN32
    gmtime_s(&out, &t);
#else
    gmtime_r(&t, &out);
#endif
    return out;
}

}  // namespace naab
