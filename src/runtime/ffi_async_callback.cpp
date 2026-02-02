// Phase 1 Item 10: FFI Async Callback Safety
// Implementation of thread-safe async callbacks

#include "naab/ffi_async_callback.h"
#include "naab/audit_logger.h"
#include <fmt/format.h>
#include <thread>
#include <algorithm>

namespace naab {
namespace ffi {

// ============================================================================
// AsyncCallbackWrapper Implementation
// ============================================================================

AsyncCallbackWrapper::AsyncCallbackWrapper(
    CallbackFunc callback,
    const std::string& name,
    std::chrono::milliseconds timeout
)
    : callback_(std::move(callback))
    , name_(name)
    , timeout_(timeout)
{
    logAsyncEvent("created", "Async callback wrapper initialized");
}

AsyncCallbackWrapper::~AsyncCallbackWrapper() {
    // Cancel if still running
    if (!done_.load()) {
        cancel();
    }
}

std::future<AsyncCallbackResult> AsyncCallbackWrapper::executeAsync() {
    logAsyncEvent("execute_async", "Starting async execution");

    return std::async(std::launch::async, [this]() {
        return executeWithTimeout();
    });
}

AsyncCallbackResult AsyncCallbackWrapper::executeBlocking() {
    logAsyncEvent("execute_blocking", "Starting blocking execution");

    auto future = executeAsync();
    return future.get();
}

void AsyncCallbackWrapper::cancel() {
    std::lock_guard<std::mutex> lock(state_mutex_);

    if (!done_.load()) {
        cancelled_.store(true);
        logAsyncEvent("cancelled", "Execution cancelled by user");

        // Log security event
        security::AuditLogger::logSecurityViolation(
            fmt::format("async_callback_cancelled: Async callback '{}' was cancelled", name_)
        );
    }
}

bool AsyncCallbackWrapper::isDone() const {
    return done_.load();
}

bool AsyncCallbackWrapper::isCancelled() const {
    return cancelled_.load();
}

AsyncCallbackResult AsyncCallbackWrapper::executeWithTimeout() {
    auto start_time = std::chrono::steady_clock::now();

    try {
        // Check for cancellation before starting
        if (cancelled_.load()) {
            return AsyncCallbackResult::makeError(
                "Callback cancelled before execution",
                "CancelledException"
            );
        }

        // Execute callback in a separate thread with timeout
        // Use shared_ptr so detached threads don't access destroyed promise
        auto result_promise = std::make_shared<std::promise<interpreter::Value>>();
        std::future<interpreter::Value> result_future = result_promise->get_future();

        std::thread worker_thread([this, result_promise]() {
            try {
                if (cancelled_.load()) {
                    result_promise->set_exception(
                        std::make_exception_ptr(
                            AsyncCallbackException("Callback cancelled during execution")
                        )
                    );
                    return;
                }

                // Execute the actual callback
                interpreter::Value result = callback_();
                result_promise->set_value(result);

            } catch (const std::exception& e) {
                result_promise->set_exception(std::current_exception());
            } catch (...) {
                result_promise->set_exception(
                    std::make_exception_ptr(
                        AsyncCallbackException("Unknown exception in callback")
                    )
                );
            }
        });

        // Wait for result with timeout
        std::future_status status;
        if (timeout_.count() > 0) {
            status = result_future.wait_for(timeout_);
        } else {
            result_future.wait();
            status = std::future_status::ready;
        }

        // Handle timeout
        if (status == std::future_status::timeout) {
            cancelled_.store(true);

            // Detach thread (can't safely cancel it)
            worker_thread.detach();

            auto end_time = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                end_time - start_time
            );

            logAsyncEvent("timeout", fmt::format(
                "Execution timed out after {}ms (limit: {}ms)",
                elapsed.count(), timeout_.count()
            ));

            // Log security violation
            security::AuditLogger::logSecurityViolation(
                fmt::format("async_callback_timeout: Async callback '{}' timed out after {}ms",
                           name_, elapsed.count())
            );

            done_.store(true);
            return AsyncCallbackResult::makeError(
                fmt::format("Callback timed out after {}ms", elapsed.count()),
                "TimeoutException"
            );
        }

        // Get result (may throw if callback threw)
        interpreter::Value result;
        try {
            result = result_future.get();

            // Join thread after successful get
            worker_thread.join();

            auto end_time = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                end_time - start_time
            );

            done_.store(true);

            logAsyncEvent("completed", fmt::format(
                "Execution completed successfully in {}ms", elapsed.count()
            ));

            return AsyncCallbackResult::makeSuccess(result, elapsed);

        } catch (const std::exception& e) {
            // Exception from callback - join thread before handling
            worker_thread.join();

            auto end_time = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                end_time - start_time
            );

            done_.store(true);

            logAsyncEvent("error", fmt::format(
                "Execution failed: {} (after {}ms)", e.what(), elapsed.count()
            ));

            // Log security violation
            security::AuditLogger::logSecurityViolation(
                fmt::format("async_callback_exception: Async callback '{}' threw exception: {}",
                           name_, e.what())
            );

            return AsyncCallbackResult::makeError(e.what(), "std::exception");
        }

    } catch (const std::exception& e) {
        // Outer catch for other exceptions (shouldn't normally happen)
        auto end_time = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time
        );

        done_.store(true);

        logAsyncEvent("error", fmt::format(
            "Unexpected error: {} (after {}ms)", e.what(), elapsed.count()
        ));

        return AsyncCallbackResult::makeError(e.what(), "std::exception");

    } catch (...) {
        done_.store(true);

        logAsyncEvent("error", "Execution failed with unknown exception");

        return AsyncCallbackResult::makeError(
            "Unknown exception in async callback",
            "UnknownException"
        );
    }
}

void AsyncCallbackWrapper::logAsyncEvent(
    const std::string& event,
    const std::string& details
) const {
    security::AuditLogger::log(
        security::AuditEvent::BLOCK_EXECUTE,
        fmt::format("[{}] {}: {}", name_, event, details)
    );
}

// ============================================================================
// AsyncCallbackGuard Implementation
// ============================================================================

AsyncCallbackGuard::AsyncCallbackGuard(
    AsyncCallbackWrapper::CallbackFunc callback,
    const std::string& name,
    std::chrono::milliseconds timeout
)
    : wrapper_(std::make_unique<AsyncCallbackWrapper>(
        std::move(callback), name, timeout
    ))
{
}

AsyncCallbackGuard::~AsyncCallbackGuard() {
    // Wrapper destructor will handle cleanup
}

AsyncCallbackResult AsyncCallbackGuard::execute() {
    return wrapper_->executeBlocking();
}

void AsyncCallbackGuard::cancel() {
    wrapper_->cancel();
}

// ============================================================================
// AsyncCallbackPool Implementation
// ============================================================================

AsyncCallbackPool::AsyncCallbackPool(size_t max_concurrent)
    : max_concurrent_(max_concurrent)
{
    security::AuditLogger::log(
        security::AuditEvent::BLOCK_EXECUTE,
        fmt::format("AsyncCallbackPool created (max_concurrent={})", max_concurrent)
    );
}

AsyncCallbackPool::~AsyncCallbackPool() {
    shutdown_.store(true);
    cancelAll();
    waitAll();
}

std::future<AsyncCallbackResult> AsyncCallbackPool::submit(
    AsyncCallbackWrapper::CallbackFunc callback,
    const std::string& name,
    std::chrono::milliseconds timeout
) {
    std::unique_lock<std::mutex> lock(pool_mutex_);

    // Wait if pool is full, periodically cleaning up completed callbacks
    while (true) {
        // Clean up completed callbacks before checking
        cleanupCompleted();

        if (shutdown_.load() || active_callbacks_.size() < max_concurrent_) {
            break;
        }

        // Wait with timeout to periodically check for completed callbacks
        pool_cv_.wait_for(lock, std::chrono::milliseconds(10));
    }

    if (shutdown_.load()) {
        throw AsyncCallbackException("Pool is shutting down");
    }

    // Create wrapper and get future
    auto wrapper = std::make_unique<AsyncCallbackWrapper>(
        std::move(callback), name, timeout
    );

    auto future = wrapper->executeAsync();

    // Store wrapper
    active_callbacks_.push_back(std::move(wrapper));

    security::AuditLogger::log(
        security::AuditEvent::BLOCK_EXECUTE,
        fmt::format("Submitted '{}' to pool (active: {})", name, active_callbacks_.size())
    );

    return future;
}

void AsyncCallbackPool::cancelAll() {
    std::lock_guard<std::mutex> lock(pool_mutex_);

    for (auto& wrapper : active_callbacks_) {
        wrapper->cancel();
    }

    security::AuditLogger::log(
        security::AuditEvent::BLOCK_EXECUTE,
        fmt::format("Cancelled all callbacks in pool (count: {})", active_callbacks_.size())
    );
}

void AsyncCallbackPool::waitAll(std::chrono::milliseconds max_wait) {
    auto start_time = std::chrono::steady_clock::now();

    while (true) {
        {
            std::lock_guard<std::mutex> lock(pool_mutex_);
            cleanupCompleted();

            if (active_callbacks_.empty()) {
                break;
            }
        }

        // Check timeout
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - start_time
        );

        if (elapsed >= max_wait) {
            security::AuditLogger::logSecurityViolation(
                fmt::format("async_pool_wait_timeout: waitAll() timed out after {}ms", elapsed.count())
            );
            break;
        }

        // Small sleep to avoid busy waiting
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

size_t AsyncCallbackPool::getActiveCount() const {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    return active_callbacks_.size();
}

size_t AsyncCallbackPool::getCompletedCount() const {
    return completed_count_.load();
}

void AsyncCallbackPool::cleanupCompleted() {
    // Remove completed callbacks
    auto it = std::remove_if(
        active_callbacks_.begin(),
        active_callbacks_.end(),
        [this](const std::unique_ptr<AsyncCallbackWrapper>& wrapper) {
            if (wrapper->isDone()) {
                completed_count_.fetch_add(1);
                pool_cv_.notify_one();
                return true;
            }
            return false;
        }
    );

    active_callbacks_.erase(it, active_callbacks_.end());
}

// ============================================================================
// Helper Functions
// ============================================================================

AsyncCallbackResult executeWithRetry(
    AsyncCallbackWrapper::CallbackFunc callback,
    const std::string& name,
    size_t max_retries,
    std::chrono::milliseconds retry_delay
) {
    size_t attempts = 0;

    while (attempts <= max_retries) {
        AsyncCallbackWrapper wrapper(callback, name);
        AsyncCallbackResult result = wrapper.executeBlocking();

        if (result.success) {
            security::AuditLogger::log(
                security::AuditEvent::BLOCK_EXECUTE,
                fmt::format("'{}' succeeded after {} attempts", name, attempts + 1)
            );
            return result;
        }

        attempts++;

        if (attempts <= max_retries) {
            security::AuditLogger::log(
                security::AuditEvent::BLOCK_EXECUTE,
                fmt::format("'{}' failed (attempt {}/{}), retrying in {}ms",
                           name, attempts, max_retries + 1, retry_delay.count())
            );

            std::this_thread::sleep_for(retry_delay);
        }
    }

    // All retries exhausted
    security::AuditLogger::logSecurityViolation(
        fmt::format("async_callback_retry_exhausted: '{}' failed after {} attempts", name, max_retries + 1)
    );

    return AsyncCallbackResult::makeError(
        fmt::format("All {} retry attempts failed", max_retries + 1),
        "RetryExhaustedException"
    );
}

std::vector<AsyncCallbackResult> executeParallel(
    const std::vector<AsyncCallbackWrapper::CallbackFunc>& callbacks,
    const std::string& group_name,
    std::chrono::milliseconds timeout
) {
    std::vector<std::unique_ptr<AsyncCallbackWrapper>> wrappers;
    std::vector<std::future<AsyncCallbackResult>> futures;
    wrappers.reserve(callbacks.size());
    futures.reserve(callbacks.size());

    // Launch all callbacks
    for (size_t i = 0; i < callbacks.size(); ++i) {
        std::string name = fmt::format("{}[{}]", group_name, i);

        auto wrapper = std::make_unique<AsyncCallbackWrapper>(
            callbacks[i], name, timeout
        );

        futures.push_back(wrapper->executeAsync());

        // Keep wrapper alive until all operations complete
        wrappers.push_back(std::move(wrapper));
    }

    // Collect all results
    std::vector<AsyncCallbackResult> results;
    results.reserve(callbacks.size());

    for (auto& future : futures) {
        results.push_back(future.get());
    }

    security::AuditLogger::log(
        security::AuditEvent::BLOCK_EXECUTE,
        fmt::format("Parallel group '{}' completed ({} callbacks)",
                   group_name, callbacks.size())
    );

    return results;
}

AsyncCallbackResult executeRace(
    const std::vector<AsyncCallbackWrapper::CallbackFunc>& callbacks,
    const std::string& group_name,
    std::chrono::milliseconds timeout
) {
    if (callbacks.empty()) {
        return AsyncCallbackResult::makeError(
            "No callbacks provided to race",
            "EmptyRaceException"
        );
    }

    std::vector<std::unique_ptr<AsyncCallbackWrapper>> wrappers;
    std::vector<std::future<AsyncCallbackResult>> futures;
    wrappers.reserve(callbacks.size());
    futures.reserve(callbacks.size());

    // Launch all callbacks
    for (size_t i = 0; i < callbacks.size(); ++i) {
        std::string name = fmt::format("{}[{}]", group_name, i);

        auto wrapper = std::make_unique<AsyncCallbackWrapper>(
            callbacks[i], name, timeout
        );

        futures.push_back(wrapper->executeAsync());

        // Keep wrapper alive until race completes
        wrappers.push_back(std::move(wrapper));
    }

    // Wait for first to complete successfully
    auto start_time = std::chrono::steady_clock::now();

    while (true) {
        // Check each future
        for (size_t i = 0; i < futures.size(); ++i) {
            if (futures[i].wait_for(std::chrono::milliseconds(0)) ==
                std::future_status::ready) {

                AsyncCallbackResult result = futures[i].get();

                if (result.success) {
                    security::AuditLogger::log(
                        security::AuditEvent::BLOCK_EXECUTE,
                        fmt::format("Race group '{}' won by callback {}", group_name, i)
                    );
                    return result;
                }
            }
        }

        // Check overall timeout
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - start_time
        );

        if (elapsed >= timeout) {
            security::AuditLogger::logSecurityViolation(
                fmt::format("async_race_timeout: Race group '{}' timed out after {}ms",
                           group_name, elapsed.count())
            );

            return AsyncCallbackResult::makeError(
                fmt::format("Race timed out after {}ms", elapsed.count()),
                "RaceTimeoutException"
            );
        }

        // Small sleep to avoid busy waiting
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

} // namespace ffi
} // namespace naab
