// Phase 1 Item 10: FFI Async Callback Safety
// Implementation of thread-safe async callbacks

#include "naab/ffi_async_callback.h"
#include "naab/audit_logger.h"
#include <fmt/format.h>
#include <thread>
#include <algorithm>
#include <iostream>

namespace naab {
namespace ffi {

// ============================================================================
// AsyncCallbackWrapper Implementation
// ============================================================================

struct AsyncCallbackWrapper::State {
    AsyncCallbackWrapper::CallbackFunc callback;
    AsyncCallbackWrapper::TaintReporterFunc taint_reporter;  // V-GOV-015: optional
    std::string name;
    std::chrono::milliseconds timeout;
    std::atomic<bool> cancelled{false};
    std::atomic<bool> done{false};
};

static void logAsyncEvent(const std::string& name, const std::string& event,
                          const std::string& details) {
    security::AuditLogger::log(
        security::AuditEvent::BLOCK_EXECUTE,
        fmt::format("[{}] {}: {}", name, event, details)
    );
}

// Run the callback on its own thread and wait up to the timeout. Takes the
// state by shared_ptr: the callback thread holds its own reference, so a
// timed-out callback that is still running when the wrapper is destroyed runs
// against live state, not a freed wrapper.
static AsyncCallbackResult runWithTimeout(std::shared_ptr<AsyncCallbackWrapper::State> st) {
    auto start_time = std::chrono::steady_clock::now();
    auto elapsed_ms = [&start_time]() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time);
    };

    try {
        if (st->cancelled.load()) {
            st->done.store(true);
            return AsyncCallbackResult::makeError(
                "Callback cancelled before execution",
                "CancelledException"
            );
        }

        auto result_promise = std::make_shared<std::promise<interpreter::NaabVal>>();
        std::future<interpreter::NaabVal> result_future = result_promise->get_future();

        std::thread worker_thread([st, result_promise]() {
            try {
                if (st->cancelled.load()) {
                    result_promise->set_exception(std::make_exception_ptr(
                        AsyncCallbackException("Callback cancelled during execution")));
                    return;
                }
                result_promise->set_value(st->callback());
            } catch (...) {
                result_promise->set_exception(std::current_exception());
            }
        });

        std::future_status status;
        if (st->timeout.count() > 0) {
            status = result_future.wait_for(st->timeout);
        } else {
            result_future.wait();
            status = std::future_status::ready;
        }

        if (status == std::future_status::timeout) {
            st->cancelled.store(true);
            worker_thread.detach();  // cannot be cancelled; it owns what it touches
            auto elapsed = elapsed_ms();
            logAsyncEvent(st->name, "timeout", fmt::format(
                "Execution timed out after {}ms (limit: {}ms)",
                elapsed.count(), st->timeout.count()));
            security::AuditLogger::logSecurityViolation(
                fmt::format("async_callback_timeout: Async callback '{}' timed out after {}ms",
                            st->name, elapsed.count()));
            st->done.store(true);
            return AsyncCallbackResult::makeError(
                fmt::format("Callback timed out after {}ms", elapsed.count()),
                "TimeoutException"
            );
        }

        worker_thread.join();
        try {
            interpreter::NaabVal result = result_future.get();
            auto elapsed = elapsed_ms();
            logAsyncEvent(st->name, "completed", fmt::format(
                "Execution completed successfully in {}ms", elapsed.count()));
            // V-GOV-015: query taint reporter (if wired) so the async result
            // carries the governance taint bit from the worker thread.
            bool tainted = st->taint_reporter ? st->taint_reporter() : false;
            st->done.store(true);
            return AsyncCallbackResult::makeSuccess(result, elapsed, tainted);
        } catch (const std::exception& e) {
            auto elapsed = elapsed_ms();
            logAsyncEvent(st->name, "error", fmt::format(
                "Execution failed: {} (after {}ms)", e.what(), elapsed.count()));
            security::AuditLogger::logSecurityViolation(
                fmt::format("async_callback_exception: Async callback '{}' threw exception: {}",
                            st->name, e.what()));
            st->done.store(true);
            return AsyncCallbackResult::makeError(e.what(), "std::exception");
        } catch (...) {
            st->done.store(true);
            return AsyncCallbackResult::makeError(
                "Unknown exception in callback", "UnknownException");
        }
    } catch (const std::exception& e) {
        logAsyncEvent(st->name, "error", fmt::format(
            "Unexpected error: {} (after {}ms)", e.what(), elapsed_ms().count()));
        st->done.store(true);
        return AsyncCallbackResult::makeError(e.what(), "std::exception");
    } catch (...) {
        logAsyncEvent(st->name, "error", "Execution failed with unknown exception");
        st->done.store(true);
        return AsyncCallbackResult::makeError(
            "Unknown exception in async callback", "UnknownException");
    }
}

AsyncCallbackWrapper::AsyncCallbackWrapper(
    CallbackFunc callback,
    const std::string& name,
    std::chrono::milliseconds timeout
)
    : name_(name)
    , timeout_(timeout)
    , state_(std::make_shared<State>())
{
    state_->callback = std::move(callback);
    state_->name = name;
    state_->timeout = timeout;
    logAsyncEvent(name_, "created", "Async callback wrapper initialized");
}

AsyncCallbackWrapper::~AsyncCallbackWrapper() {
    // Cancel if still running
    if (!state_->done.load()) {
        cancel();
    }
}

void AsyncCallbackWrapper::setTaintReporter(TaintReporterFunc reporter) {
    state_->taint_reporter = std::move(reporter);
}

std::future<AsyncCallbackResult> AsyncCallbackWrapper::executeAsync() {
    logAsyncEvent(name_, "execute_async", "Starting async execution");

    // Starts NOW. This used std::launch::deferred (commented as a workaround
    // for thread exhaustion), so a callback ran only when someone called its
    // future's .get(): AsyncCallbackPool, which blocks submit() until an
    // earlier callback finishes, deadlocked once submissions exceeded its
    // limit, and executeRace() polled futures that could never become ready.
    auto st = state_;
    auto promise = std::make_shared<std::promise<AsyncCallbackResult>>();
    auto future = promise->get_future();
    std::thread([st, promise]() {
        promise->set_value(runWithTimeout(st));
    }).detach();
    return future;
}

AsyncCallbackResult AsyncCallbackWrapper::executeBlocking() {
    logAsyncEvent(name_, "execute_blocking", "Starting blocking execution");
    return runWithTimeout(state_);
}

void AsyncCallbackWrapper::cancel() {
    std::lock_guard<std::mutex> lock(state_mutex_);

    if (!state_->done.load()) {
        state_->cancelled.store(true);
        logAsyncEvent(name_, "cancelled", "Execution cancelled by user");

        // Log security event
        security::AuditLogger::logSecurityViolation(
            fmt::format("async_callback_cancelled: Async callback '{}' was cancelled", name_)
        );
    }
}

bool AsyncCallbackWrapper::isDone() const {
    return state_->done.load();
}

bool AsyncCallbackWrapper::isCancelled() const {
    return state_->cancelled.load();
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
