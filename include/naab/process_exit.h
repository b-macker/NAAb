#pragma once
// exitProcess(): how a CLI leaves through _exit() without losing evidence.
//
// The CLIs end a run with _exit(), never a return from main: a HARD block is
// _exit(3), and even a clean run ends in _exit(0) (thread pools and Python
// thread states crash during static destruction on some platforms). _exit()
// runs no destructors, so ~GovernanceEngine's telemetry-forwarder drain never
// ran, and every event still queued for the webhook was lost -- measured on a
// clean run: 0 of its 3 events reached the webhook (F53). Every _exit() in the
// run path goes through here instead, so the queue is drained first, within
// telemetry.forward_shutdown_drain_ms.
//
// Not for a forked child before exec: it shares the parent's forwarders by
// copy and must not post them.

#include <naab/telemetry_forwarder.h>
#include <cstdio>
#include <cstdlib>
#ifdef _WIN32
#  include <process.h>  // _exit()
#else
#  include <unistd.h>   // _exit()
#endif

namespace naab {

[[noreturn]] inline void exitProcess(int code) {
    TelemetryForwarder::drainAll();
    std::fflush(nullptr);
    _exit(code);
}

}  // namespace naab
