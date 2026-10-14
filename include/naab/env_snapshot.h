// NAAb load-time environment snapshot -- the ONE copy of the process
// environment that operator-owned connections and processes read.
//
// A NAAb program's env.set writes the real process environment (env writes are
// allowed by default). Anything the engine does on the OPERATOR's behalf must
// therefore not read the live environment: an agent call (the API key), the
// telemetry webhook, a guidance source (fetched over https, or run as a command
// with the environment it was given) -- otherwise the program chooses their
// proxy, their trust store, their PATH. The program's OWN http requests take
// their proxy from here too, so it cannot route them through a host the
// network policy never lists.
//
// The snapshot is taken once per process, before any program statement runs:
// at the first config load (loadFromJson) or when `naab-lang run` enables
// external guidance, whichever comes first. Both happen before the program
// starts. A later reload does not re-capture, which is correct -- a reload
// runs after user code. Readers that run in a process that never loaded a
// config capture on first use (a backstop, not the guarantee).
//
// History: guidance connectors (#312) and the agent/telemetry fix (#313) each
// built their own snapshot, captured at different points and disagreeing on
// one variable (upper-case HTTP_PROXY). This file replaces both; the proxy and
// CA rules on top of it are in net_proxy_snapshot.h.

#pragma once

#include <string>
#include <vector>

namespace naab {

// Capture the process environment if it has not been captured yet. Cheap
// after the first call.
void captureEnvironmentSnapshotOnce();

// The captured environment, as "KEY=value" strings. Never changes after the
// capture; safe to read from any thread.
const std::vector<std::string>& environmentSnapshot();

// One variable from the snapshot, "" when absent. Exact name; on Windows,
// where variable names are case-insensitive, a case-insensitive match after
// that (as getenv behaves there).
std::string snapshotEnv(const std::string& key);

} // namespace naab
