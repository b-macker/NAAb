#pragma once
// A governed program run in a fresh `naab-lang run` worker process.
//
// Shared by `naab-lang mcp` and REST /execute: no user code runs in the
// server process, so interpreter state, data and crashes do not cross calls,
// and every program runs on the default engine under the project's
// govern.json (docs/plan-mcp-service.md). POSIX only: the worker gets its own
// process group, and whatever it leaves there is killed when it exits.
#ifndef _WIN32

#include <sys/types.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace naab {
namespace api {

constexpr size_t kWorkerMaxOutput = 1024u * 1024u;      // program stdout kept
constexpr size_t kWorkerMaxDiagnostics = 64u * 1024u;   // worker stderr kept
constexpr size_t kWorkerMaxEventLine = 4096;

// A call in progress, so another thread can cancel it.
struct InFlight {
    std::mutex mu;
    pid_t pid = -1;
    bool cancelled = false;
};

struct WorkerResult {
    bool started = false;
    int exit_code = -1;      // valid when exited
    int term_signal = 0;     // valid when signalled
    bool timed_out = false;
    bool cancelled = false;
    std::string out;
    bool out_truncated = false;
    std::string err;
    int events = 0;          // lines on_event accepted
};

// Start `self run <args>` in cwd, in its own process group, and collect its
// stdout, stderr and the event stream on descriptor 3 until it exits, is
// cancelled, or runs out of time (timeout_seconds, plus 5 s for the worker's
// own timer to stop it first). on_event gets each line of the event stream
// and returns whether it was an event.
WorkerResult runWorker(const std::string& self, const std::string& cwd,
                       const std::vector<std::string>& args, int timeout_seconds,
                       const std::shared_ptr<InFlight>& fl,
                       const std::function<bool(const std::string&)>& on_event);

std::string randomHex(size_t bytes);

// Write a program to DIR/<prefix><random>.naab (mode 0600, never an existing
// file) and return its path, or "" on failure. In the server's directory, so
// relative imports resolve and discovery finds the same govern.json as for a
// script there.
std::string writeWorkerProgram(const std::string& dir, const std::string& prefix,
                               const std::string& code);

// Program files are removed after every call; only a server that was killed
// leaves one. Another server may be working in the same directory, so only
// files older than any call could run are removed.
void removeStaleWorkerPrograms(const std::string& dir, const std::string& prefix);

}  // namespace api
}  // namespace naab

#endif  // _WIN32
