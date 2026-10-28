#include <naab/telemetry_forwarder.h>
#include <naab/curl_egress.h>
#include <filesystem>
#include <curl/curl.h>
#include <cstdio>
#include <chrono>
#include <vector>
#include <string>
#include <set>
#include <algorithm>

namespace naab {

// Discard response body — we only care about the status code
static size_t discardCallback(char*, size_t size, size_t nmemb, void*) {
    return size * nmemb;
}

// Every forwarder with a running worker, so drainAll() can reach them before
// _exit(). A forwarder leaves the registry only after its own shutdown, and
// drainAll() holds the registry lock while it drains, so neither can outlive
// the other's use of it.
static std::mutex& registryMutex() { static std::mutex m; return m; }
static std::set<TelemetryForwarder*>& registry() { static std::set<TelemetryForwarder*> r; return r; }

static long long ticksOf(std::chrono::steady_clock::time_point t) {
    return static_cast<long long>(t.time_since_epoch().count());
}
static std::chrono::steady_clock::time_point timeOf(long long ticks) {
    return std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(ticks));
}

TelemetryForwarder::TelemetryForwarder(const TelemetryForwarderConfig& config)
    : config_(config) {
    if (config_.webhook_url.empty()) return;
    worker_ = std::thread(&TelemetryForwarder::workerLoop, this);
    std::lock_guard<std::mutex> rlock(registryMutex());
    registry().insert(this);
}

TelemetryForwarder::~TelemetryForwarder() {
    shutdown();
    std::lock_guard<std::mutex> rlock(registryMutex());
    registry().erase(this);
}

void TelemetryForwarder::enqueue(const std::string& json_line) {
    if (!running_.load() || config_.webhook_url.empty()) return;

    std::lock_guard<std::mutex> lock(mutex_);
    if (config_.buffer_max > 0 &&
        static_cast<int>(buffer_.size()) >= config_.buffer_max) {
        // Drop oldest to make room
        buffer_.pop_front();
        events_dropped_++;
    }
    buffer_.push_back(json_line);
    cv_.notify_one();
}

void TelemetryForwarder::shutdown() {
    shutdownBy(std::chrono::steady_clock::now() +
               std::chrono::milliseconds(config_.shutdown_drain_ms), /*join=*/true);
}

void TelemetryForwarder::drainAll() {
    std::lock_guard<std::mutex> rlock(registryMutex());
    int drain_ms = 0;
    for (auto* f : registry()) drain_ms = std::max(drain_ms, f->config_.shutdown_drain_ms);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(drain_ms);
    for (auto* f : registry()) f->shutdownBy(deadline, /*join=*/false);
}

void TelemetryForwarder::shutdownBy(std::chrono::steady_clock::time_point deadline, bool join) {
    // A second caller (the destructor after drainAll, or drainAll during a
    // reload's teardown) waits for the first instead of returning while it is
    // still draining.
    std::lock_guard<std::mutex> slock(shutdown_mutex_);
    if (shut_down_) {
        if (join && worker_.joinable()) worker_.join();
        return;
    }
    shut_down_ = true;
    deadline_ticks_.store(ticksOf(deadline));
    running_.store(false);
    cv_.notify_all();

    // The worker may be posting a batch it already took off the buffer: give
    // it until the deadline (its POST is capped by the same deadline).
    size_t abandoned = 0;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!idle_cv_.wait_until(lock, deadline, [this] { return !worker_busy_; }))
            abandoned = in_flight_;   // still posting at the deadline: lost with the exit
    }

    // M2: Final drain -- post what is still buffered, by the deadline. The
    // deadline is checked before each batch and caps each POST's timeout, so
    // one slow webhook cannot hold the exit past it.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!buffer_.empty()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                abandoned += buffer_.size();
                events_dropped_ += buffer_.size();
                buffer_.clear();
                break;
            }
            std::string payload = "[";
            int count = 0;
            while (!buffer_.empty() && count < config_.batch_size) {
                if (count > 0) payload += ",";
                payload += buffer_.front();
                buffer_.pop_front();
                count++;
            }
            payload += "]";
            if (postBatch(payload)) {
                events_forwarded_ += static_cast<size_t>(count);
            } else {
                events_dropped_ += static_cast<size_t>(count);
            }
        }
    }

    if (abandoned > 0) {
        fprintf(stderr, "[telemetry] Shutdown drain timeout: %zu events not forwarded\n", abandoned);
    }

    if (worker_.joinable()) {
        // A destructor must join (the worker uses this object); the exit path
        // must not wait past its deadline for a request still in flight.
        if (join) worker_.join();
        else worker_.detach();
    }
}

void TelemetryForwarder::workerLoop() {
    while (running_.load()) {
        std::vector<std::string> batch;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            // Wait until we have events or are shutting down
            cv_.wait_for(lock, std::chrono::seconds(1), [this] {
                return !buffer_.empty() || !running_.load();
            });

            // Once shutdown has begun, what is buffered is the final drain's:
            // a batch taken here would race it past the deadline.
            if (!running_.load() || buffer_.empty()) continue;

            // Drain up to batch_size events
            int count = 0;
            while (!buffer_.empty() && count < config_.batch_size) {
                batch.push_back(std::move(buffer_.front()));
                buffer_.pop_front();
                count++;
            }
            worker_busy_ = true;
            in_flight_ = batch.size();
        }

        // Build JSON array payload
        std::string payload = "[";
        for (size_t i = 0; i < batch.size(); i++) {
            if (i > 0) payload += ",";
            payload += batch[i];
        }
        payload += "]";

        // POST with retry
        bool sent = false;
        for (int attempt = 0; attempt <= config_.retry_count; attempt++) {
            if (postBatch(payload)) {
                events_forwarded_ += batch.size();
                sent = true;
                break;
            }
            // Brief backoff before retry
            if (attempt < config_.retry_count) {
                if (!running_.load()) break;  // shutdown requested — skip remaining retries
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(100 * (1 << attempt)));
            }
        }

        if (!sent) {
            forward_errors_++;
            // Events are lost after retry exhaustion — don't re-enqueue
            // to avoid infinite loops
            events_dropped_ += batch.size();
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            worker_busy_ = false;
            in_flight_ = 0;
        }
        idle_cv_.notify_all();
    }
}

// Aborts a transfer once the shutdown deadline has passed: a POST that began
// before shutdown has a timeout that knew nothing of it.
static int deadlineProgress(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* ticks = static_cast<std::atomic<long long>*>(clientp);
    long long t = ticks->load();
    return (t != 0 && std::chrono::steady_clock::now() >= timeOf(t)) ? 1 : 0;
}

bool TelemetryForwarder::postBatch(const std::string& payload) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    curl_easy_setopt(curl, CURLOPT_URL, config_.webhook_url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(payload.size()));

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discardCallback);
    long timeout_ms = static_cast<long>(config_.timeout_ms);
    if (long long t = deadline_ticks_.load()) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            timeOf(t) - std::chrono::steady_clock::now()).count();
        if (left <= 0) { curl_easy_cleanup(curl); return false; }
        timeout_ms = std::min<long>(timeout_ms, static_cast<long>(left));
    }
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, deadlineProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &deadline_ticks_);

    // HTTPS verification
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    // Proxy and CA come from the load-time snapshot, never the live
    // environment: the webhook stream leaves the machine, so a program's env.set
    // must not move its proxy or trust store (curl_egress.h). This copy once
    // drifted from agent_provider's (it read only CURL_CA_BUNDLE, and an empty
    // variable set CAINFO to ""); both now call the same helper.
    naab::net::applySnapshotProxy(curl, config_.webhook_url);
    naab::net::applySnapshotCa(curl);

    // Headers
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (!config_.auth_header.empty()) {
        std::string auth = "Authorization: " + config_.auth_header;
        headers = curl_slist_append(headers, auth.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    long status_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) return false;
    // Accept 2xx status codes
    return (status_code >= 200 && status_code < 300);
}

} // namespace naab
