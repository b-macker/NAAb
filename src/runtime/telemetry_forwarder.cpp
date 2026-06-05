#include <naab/telemetry_forwarder.h>
#include <curl/curl.h>
#include <cstdio>
#include <chrono>

namespace naab {

// Discard response body — we only care about the status code
static size_t discardCallback(char*, size_t size, size_t nmemb, void*) {
    return size * nmemb;
}

TelemetryForwarder::TelemetryForwarder(const TelemetryForwarderConfig& config)
    : config_(config) {
    if (config_.webhook_url.empty()) return;
    worker_ = std::thread(&TelemetryForwarder::workerLoop, this);
}

TelemetryForwarder::~TelemetryForwarder() {
    shutdown();
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
    if (!running_.exchange(false)) return;  // already shut down
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }

    // Final drain — flush remaining events
    std::lock_guard<std::mutex> lock(mutex_);
    while (!buffer_.empty()) {
        std::string payload = "[";
        int count = 0;
        while (!buffer_.empty() && count < config_.batch_size) {
            if (count > 0) payload += ",";
            payload += buffer_.front();
            buffer_.pop_front();
            count++;
        }
        payload += "]";
        if (!postBatch(payload)) {
            // Final flush failed — events lost
            events_dropped_ += static_cast<size_t>(count);
        }
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

            if (buffer_.empty()) continue;

            // Drain up to batch_size events
            int count = 0;
            while (!buffer_.empty() && count < config_.batch_size) {
                batch.push_back(std::move(buffer_.front()));
                buffer_.pop_front();
                count++;
            }
        }

        if (batch.empty()) continue;

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
    }
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
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
                     static_cast<long>(config_.timeout_ms));

    // HTTPS verification
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    // CA bundle (same logic as agent_provider)
    const char* ca_env = std::getenv("CURL_CA_BUNDLE");
    if (ca_env) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, ca_env);
    } else {
        curl_easy_setopt(curl, CURLOPT_CAINFO,
                         "/data/data/com.termux/files/usr/etc/tls/cert.pem");
    }

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
