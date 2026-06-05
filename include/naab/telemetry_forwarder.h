#pragma once
// TelemetryForwarder: background thread that forwards JSONL telemetry events
// to a webhook endpoint via HTTP POST. Non-blocking enqueue, batched drain.

#include <string>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>

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

    // Flush remaining events and stop the background thread.
    void shutdown();

    // Stats
    size_t events_forwarded() const { return events_forwarded_.load(); }
    size_t events_dropped() const { return events_dropped_.load(); }
    size_t forward_errors() const { return forward_errors_.load(); }

private:
    void workerLoop();
    bool postBatch(const std::string& payload);

    TelemetryForwarderConfig config_;
    std::deque<std::string> buffer_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    std::atomic<bool> running_{true};
    std::atomic<size_t> events_forwarded_{0};
    std::atomic<size_t> events_dropped_{0};
    std::atomic<size_t> forward_errors_{0};
};

} // namespace naab
