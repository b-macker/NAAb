#pragma once

// Phase 1 Item 10: FFI Async Callback Safety
// Thread-safe async callbacks across FFI boundaries

#include "naab/value.h"
#include <string>
#include <stdexcept>
#include <functional>
#include <future>
#include <chrono>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <optional>

namespace naab {
namespace ffi {

// Use Value from interpreter namespace
using interpreter::Value;

// Exception for async callback errors
class AsyncCallbackException : public std::runtime_error {
public:
    explicit AsyncCallbackException(const std::string& msg)
        : std::runtime_error(msg) {}
};

// Result of async callback execution
struct AsyncCallbackResult {
    bool success;
    Value value;
    std::string error_message;
    std::string error_type;
    std::chrono::milliseconds execution_time;

    AsyncCallbackResult()
        : success(false)
        , value()
        , execution_time(0) {}

    static AsyncCallbackResult makeSuccess(
        const Value& val,
        std::chrono::milliseconds exec_time
    ) {
        AsyncCallbackResult result;
        result.success = true;
        result.value = val;
        result.execution_time = exec_time;
        return result;
    }

    static AsyncCallbackResult makeError(
        const std::string& error_msg,
        const std::string& error_type_str
    ) {
        AsyncCallbackResult result;
        result.success = false;
        result.error_message = error_msg;
        result.error_type = error_type_str;
        return result;
    }
};

// Thread-safe wrapper for async callbacks
class AsyncCallbackWrapper {
public:
    using CallbackFunc = std::function<Value()>;

    // Create async callback wrapper
    // timeout_ms: Maximum execution time (0 = no timeout)
    explicit AsyncCallbackWrapper(
        CallbackFunc callback,
        const std::string& name = "async_callback",
        std::chrono::milliseconds timeout = std::chrono::milliseconds(30000)
    );

    ~AsyncCallbackWrapper();

    // Non-copyable and non-movable (manages thread state and mutex)
    AsyncCallbackWrapper(const AsyncCallbackWrapper&) = delete;
    AsyncCallbackWrapper& operator=(const AsyncCallbackWrapper&) = delete;
    AsyncCallbackWrapper(AsyncCallbackWrapper&&) = delete;
    AsyncCallbackWrapper& operator=(AsyncCallbackWrapper&&) = delete;

    // Execute callback asynchronously
    std::future<AsyncCallbackResult> executeAsync();

    // Execute callback with blocking wait for result
    AsyncCallbackResult executeBlocking();

    // Cancel ongoing execution (if possible)
    void cancel();

    // Check if execution is complete
    bool isDone() const;

    // Check if execution was cancelled
    bool isCancelled() const;

    // Get callback name (for logging/debugging)
    const std::string& getName() const { return name_; }

    // Get timeout setting
    std::chrono::milliseconds getTimeout() const { return timeout_; }

private:
    CallbackFunc callback_;
    std::string name_;
    std::chrono::milliseconds timeout_;

    mutable std::mutex state_mutex_;
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> done_{false};

    // Internal execution with timeout
    AsyncCallbackResult executeWithTimeout();

    // Logging helper
    void logAsyncEvent(const std::string& event, const std::string& details) const;
};

// RAII guard for async callback execution
// Ensures proper cleanup even if exception thrown
class AsyncCallbackGuard {
public:
    AsyncCallbackGuard(
        AsyncCallbackWrapper::CallbackFunc callback,
        const std::string& name,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(30000)
    );

    ~AsyncCallbackGuard();

    // Execute and get result
    AsyncCallbackResult execute();

    // Cancel execution
    void cancel();

private:
    std::unique_ptr<AsyncCallbackWrapper> wrapper_;
};

// Async callback pool for managing multiple concurrent callbacks
class AsyncCallbackPool {
public:
    AsyncCallbackPool(size_t max_concurrent = 10);
    ~AsyncCallbackPool();

    // Submit callback for async execution
    std::future<AsyncCallbackResult> submit(
        AsyncCallbackWrapper::CallbackFunc callback,
        const std::string& name = "pooled_callback",
        std::chrono::milliseconds timeout = std::chrono::milliseconds(30000)
    );

    // Cancel all pending callbacks
    void cancelAll();

    // Wait for all callbacks to complete
    void waitAll(std::chrono::milliseconds max_wait = std::chrono::milliseconds(60000));

    // Get number of active callbacks
    size_t getActiveCount() const;

    // Get number of completed callbacks
    size_t getCompletedCount() const;

private:
    size_t max_concurrent_;

    mutable std::mutex pool_mutex_;
    std::condition_variable pool_cv_;

    std::vector<std::unique_ptr<AsyncCallbackWrapper>> active_callbacks_;
    std::atomic<size_t> completed_count_{0};
    std::atomic<bool> shutdown_{false};

    // Cleanup completed callbacks
    void cleanupCompleted();
};

// Helper functions for common async patterns

// Execute callback with retry on failure
AsyncCallbackResult executeWithRetry(
    AsyncCallbackWrapper::CallbackFunc callback,
    const std::string& name,
    size_t max_retries = 3,
    std::chrono::milliseconds retry_delay = std::chrono::milliseconds(100)
);

// Execute multiple callbacks in parallel, return when all complete
std::vector<AsyncCallbackResult> executeParallel(
    const std::vector<AsyncCallbackWrapper::CallbackFunc>& callbacks,
    const std::string& group_name = "parallel_group",
    std::chrono::milliseconds timeout = std::chrono::milliseconds(30000)
);

// Execute multiple callbacks, return when first completes successfully
AsyncCallbackResult executeRace(
    const std::vector<AsyncCallbackWrapper::CallbackFunc>& callbacks,
    const std::string& group_name = "race_group",
    std::chrono::milliseconds timeout = std::chrono::milliseconds(30000)
);

} // namespace ffi
} // namespace naab

