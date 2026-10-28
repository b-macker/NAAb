#pragma once
// TelemetryForwarder: background thread that forwards JSONL telemetry events
// to a webhook endpoint via HTTP POST. Non-blocking enqueue, batched drain.

#include <string>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <chrono>

namespace naab {

struct TelemetryForwarderConfig {
    std::string webhook_url;
    std::string auth_header;       // e.g. "Bearer xxx"
    int batch_size = 10;
    int timeout_ms = 5000;
    int retry_count = 2;
    int buffer_max = 1000;
    int shutdown_drain_ms = 5000;  // M2: max wall-clock time for final drain (0 = skip drain)
};

class TelemetryForwarder {
public:
    explicit TelemetryForwarder(const TelemetryForwarderConfig& config);
    ~TelemetryForwarder();

    // Non-copyable, non-movable (owns thread)
    TelemetryForwarder(const TelemetryForwarder&) = delete;
    TelemetryForwarder& operator=(const TelemetryForwarder&) = delete;

    // Enqueue a JSON line for forwarding. Non-blocking.
    // Drops oldest events if buffer is full.
    void enqueue(const std::string& json_line);

    // Flush remaining events and stop the background thread, within
    // shutdown_drain_ms (the destructor's path: joins the worker).
    void shutdown();

    // Drain every live forwarder before the process leaves through _exit(),
    // which skips the destructors that would otherwise drain them (F53). One
    // deadline for all: now + the largest shutdown_drain_ms. The worker is not
    // joined -- a request it has in flight past the deadline is abandoned with
    // the process -- so the call returns by the deadline. Idempotent.
    static void drainAll();

    // Stats
    size_t events_forwarded() const { return events_forwarded_.load(); }
    size_t events_dropped() const { return events_dropped_.load(); }
    size_t forward_errors() const { return forward_errors_.load(); }

private:
    void workerLoop();
    bool postBatch(const std::string& payload);
    // Stops the worker and posts what is buffered, all by `deadline`; joins
    // the worker when `join` (a destructor must), else leaves it to the exit.
    void shutdownBy(std::chrono::steady_clock::time_point deadline, bool join);

    TelemetryForwarderConfig config_;
    std::deque<std::string> buffer_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable idle_cv_;   // the worker finished its in-flight batch
    bool worker_busy_ = false;          // guarded by mutex_
    size_t in_flight_ = 0;              // events in that batch; guarded by mutex_
    std::thread worker_;
    std::atomic<bool> running_{true};
    // Shutdown deadline (steady_clock ticks since epoch; 0 = none). Every POST
    // caps its own timeout at the time left, and one already in flight when
    // the deadline is set aborts through curl's progress callback once it passes.
    std::atomic<long long> deadline_ticks_{0};
    std::mutex shutdown_mutex_;         // a second shutdown waits for the first
    bool shut_down_ = false;            // guarded by shutdown_mutex_
    std::atomic<size_t> events_forwarded_{0};
    std::atomic<size_t> events_dropped_{0};
    std::atomic<size_t> forward_errors_{0};
};

} // namespace naab
