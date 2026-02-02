// Phase 1 Item 10: FFI Async Callback Safety - Unit Tests
// Tests for thread-safe async callback framework

#include "naab/ffi_async_callback.h"
#include "naab/value.h"
#include <gtest/gtest.h>
#include <fmt/format.h>
#include <thread>
#include <chrono>
#include <vector>

using namespace naab::ffi;
using namespace naab::interpreter;

class FFIAsyncCallbackTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Initialize test fixtures
    }

    void TearDown() override {
        // Cleanup
    }

    // Helper: Create simple callback that returns a value
    static AsyncCallbackWrapper::CallbackFunc makeSimpleCallback(int value) {
        return [value]() -> Value {
            return Value(value);
        };
    }

    // Helper: Create callback that sleeps then returns
    static AsyncCallbackWrapper::CallbackFunc makeSleepCallback(
        int sleep_ms,
        int return_value
    ) {
        return [sleep_ms, return_value]() -> Value {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
            return Value(return_value);
        };
    }

    // Helper: Create callback that throws exception
    static AsyncCallbackWrapper::CallbackFunc makeThrowingCallback(
        const std::string& message
    ) {
        return [message]() -> Value {
            throw std::runtime_error(message);
        };
    }
};

// ============================================================================
// Basic Async Execution Tests
// ============================================================================

TEST_F(FFIAsyncCallbackTest, SimpleBlockingExecution) {
    AsyncCallbackWrapper wrapper(
        makeSimpleCallback(42),
        "simple_test"
    );

    auto result = wrapper.executeBlocking();

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.value.toInt(), 42);
    EXPECT_TRUE(result.error_message.empty());
    EXPECT_TRUE(wrapper.isDone());
}

TEST_F(FFIAsyncCallbackTest, SimpleAsyncExecution) {
    AsyncCallbackWrapper wrapper(
        makeSimpleCallback(123),
        "async_test"
    );

    auto future = wrapper.executeAsync();
    auto result = future.get();

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.value.toInt(), 123);
    EXPECT_TRUE(wrapper.isDone());
}

TEST_F(FFIAsyncCallbackTest, ExecutionTime) {
    int sleep_ms = 50;

    AsyncCallbackWrapper wrapper(
        makeSleepCallback(sleep_ms, 99),
        "timed_test",
        std::chrono::milliseconds(1000)  // 1 second timeout
    );

    auto result = wrapper.executeBlocking();

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.value.toInt(), 99);

    // Check execution time is approximately correct
    EXPECT_GE(result.execution_time.count(), sleep_ms - 10);  // Allow 10ms margin
    EXPECT_LE(result.execution_time.count(), sleep_ms + 100); // Allow 100ms margin
}

// ============================================================================
// Exception Handling Tests
// ============================================================================

TEST_F(FFIAsyncCallbackTest, ExceptionCaught) {
    AsyncCallbackWrapper wrapper(
        makeThrowingCallback("Test error"),
        "exception_test"
    );

    auto result = wrapper.executeBlocking();

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error_message.find("Test error") != std::string::npos);
    EXPECT_EQ(result.error_type, "std::exception");
    EXPECT_TRUE(wrapper.isDone());
}

TEST_F(FFIAsyncCallbackTest, MultipleExceptionTypes) {
    // Test with std::runtime_error
    {
        AsyncCallbackWrapper wrapper(
            []() -> Value { throw std::runtime_error("runtime error"); },
            "runtime_error_test"
        );

        auto result = wrapper.executeBlocking();
        EXPECT_FALSE(result.success);
        EXPECT_TRUE(result.error_message.find("runtime error") != std::string::npos);
    }

    // Test with std::logic_error
    {
        AsyncCallbackWrapper wrapper(
            []() -> Value { throw std::logic_error("logic error"); },
            "logic_error_test"
        );

        auto result = wrapper.executeBlocking();
        EXPECT_FALSE(result.success);
        EXPECT_TRUE(result.error_message.find("logic error") != std::string::npos);
    }
}

// ============================================================================
// Timeout Tests
// ============================================================================

TEST_F(FFIAsyncCallbackTest, TimeoutTriggered) {
    // Callback sleeps for 200ms, timeout is 50ms
    AsyncCallbackWrapper wrapper(
        makeSleepCallback(200, 42),
        "timeout_test",
        std::chrono::milliseconds(50)  // 50ms timeout
    );

    auto result = wrapper.executeBlocking();

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error_message.find("timeout") != std::string::npos ||
                result.error_message.find("timed out") != std::string::npos);
    EXPECT_EQ(result.error_type, "TimeoutException");
    EXPECT_TRUE(wrapper.isDone());
}

TEST_F(FFIAsyncCallbackTest, NoTimeoutWhenFast) {
    // Callback sleeps for 10ms, timeout is 1000ms
    AsyncCallbackWrapper wrapper(
        makeSleepCallback(10, 42),
        "no_timeout_test",
        std::chrono::milliseconds(1000)
    );

    auto result = wrapper.executeBlocking();

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.value.toInt(), 42);
}

TEST_F(FFIAsyncCallbackTest, ZeroTimeoutMeansNoLimit) {
    // Callback sleeps for 100ms, timeout is 0 (unlimited)
    AsyncCallbackWrapper wrapper(
        makeSleepCallback(100, 99),
        "unlimited_test",
        std::chrono::milliseconds(0)  // No timeout
    );

    auto result = wrapper.executeBlocking();

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.value.toInt(), 99);
}

// ============================================================================
// Cancellation Tests
// ============================================================================

TEST_F(FFIAsyncCallbackTest, CancelBeforeExecution) {
    AsyncCallbackWrapper wrapper(
        makeSimpleCallback(42),
        "cancel_before_test"
    );

    wrapper.cancel();
    EXPECT_TRUE(wrapper.isCancelled());

    auto result = wrapper.executeBlocking();

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error_message.find("cancel") != std::string::npos);
}

TEST_F(FFIAsyncCallbackTest, CancelDuringExecution) {
    AsyncCallbackWrapper wrapper(
        makeSleepCallback(500, 42),  // Long sleep
        "cancel_during_test",
        std::chrono::milliseconds(2000)  // Long timeout
    );

    // Start async execution
    auto future = wrapper.executeAsync();

    // Wait a bit then cancel
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    wrapper.cancel();

    EXPECT_TRUE(wrapper.isCancelled());

    // Note: Cancellation only works before callback starts executing.
    // Once the callback is running, it cannot be interrupted (C++ limitation).
    // Since we waited 50ms before cancelling, the callback is already running,
    // so it will complete successfully despite being marked as cancelled.
    auto result = future.get();

    // The callback completes successfully, but the wrapper is marked as cancelled
    EXPECT_TRUE(result.success);  // Callback completed
    EXPECT_EQ(result.value.toInt(), 42);
    EXPECT_TRUE(wrapper.isCancelled());  // But wrapper knows it was cancelled
}

// ============================================================================
// AsyncCallbackGuard Tests (RAII)
// ============================================================================

TEST_F(FFIAsyncCallbackTest, GuardBasicExecution) {
    AsyncCallbackGuard guard(
        makeSimpleCallback(77),
        "guard_test"
    );

    auto result = guard.execute();

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.value.toInt(), 77);
}

TEST_F(FFIAsyncCallbackTest, GuardCancellation) {
    AsyncCallbackGuard guard(
        makeSleepCallback(500, 42),
        "guard_cancel_test"
    );

    guard.cancel();

    auto result = guard.execute();
    EXPECT_FALSE(result.success);
}

// ============================================================================
// AsyncCallbackPool Tests
// ============================================================================

TEST_F(FFIAsyncCallbackTest, PoolBasicSubmit) {
    AsyncCallbackPool pool(5);  // Max 5 concurrent

    auto future = pool.submit(makeSimpleCallback(11), "pool_test_1");
    auto result = future.get();

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.value.toInt(), 11);
}

TEST_F(FFIAsyncCallbackTest, PoolMultipleCallbacks) {
    AsyncCallbackPool pool(10);

    std::vector<std::future<AsyncCallbackResult>> futures;

    // Submit 5 callbacks
    for (int i = 0; i < 5; ++i) {
        futures.push_back(pool.submit(
            makeSimpleCallback(i * 10),
            fmt::format("pool_multi_{}", i)
        ));
    }

    // Collect results
    for (size_t i = 0; i < futures.size(); ++i) {
        auto result = futures[i].get();
        EXPECT_TRUE(result.success);
        EXPECT_EQ(result.value.toInt(), static_cast<int64_t>(i * 10));
    }
}

TEST_F(FFIAsyncCallbackTest, PoolConcurrencyLimit) {
    AsyncCallbackPool pool(2);  // Max 2 concurrent

    std::vector<std::future<AsyncCallbackResult>> futures;

    // Submit 4 callbacks that sleep
    for (int i = 0; i < 4; ++i) {
        futures.push_back(pool.submit(
            makeSleepCallback(50, i),
            fmt::format("pool_limit_{}", i)
        ));
    }

    // Pool should handle them in batches of 2
    for (auto& future : futures) {
        auto result = future.get();
        EXPECT_TRUE(result.success);
    }
}

TEST_F(FFIAsyncCallbackTest, PoolCancelAll) {
    AsyncCallbackPool pool(5);

    // Submit some long-running callbacks
    for (int i = 0; i < 3; ++i) {
        pool.submit(
            makeSleepCallback(500, i),
            fmt::format("pool_cancel_{}", i)
        );
    }

    // Cancel all
    pool.cancelAll();

    // Pool should be empty after waiting
    pool.waitAll(std::chrono::milliseconds(1000));

    EXPECT_EQ(pool.getActiveCount(), 0);
}

// ============================================================================
// Helper Function Tests
// ============================================================================

TEST_F(FFIAsyncCallbackTest, ExecuteWithRetrySuccess) {
    // Callback that succeeds immediately
    auto result = executeWithRetry(
        makeSimpleCallback(55),
        "retry_success",
        3  // Max 3 retries
    );

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.value.toInt(), 55);
}

TEST_F(FFIAsyncCallbackTest, ExecuteWithRetryFailure) {
    // Callback that always fails
    auto result = executeWithRetry(
        makeThrowingCallback("Always fails"),
        "retry_failure",
        2,  // Max 2 retries
        std::chrono::milliseconds(10)  // Short retry delay for testing
    );

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error_message.find("retry") != std::string::npos);
}

TEST_F(FFIAsyncCallbackTest, ExecuteParallelAll) {
    std::vector<AsyncCallbackWrapper::CallbackFunc> callbacks;

    // Create 3 callbacks
    for (int i = 0; i < 3; ++i) {
        callbacks.push_back(makeSimpleCallback(i * 100));
    }

    auto results = executeParallel(
        callbacks,
        "parallel_test"
    );

    EXPECT_EQ(results.size(), 3);

    for (size_t i = 0; i < results.size(); ++i) {
        EXPECT_TRUE(results[i].success);
        EXPECT_EQ(results[i].value.toInt(), static_cast<int64_t>(i * 100));
    }
}

TEST_F(FFIAsyncCallbackTest, ExecuteRaceFirstWins) {
    std::vector<AsyncCallbackWrapper::CallbackFunc> callbacks;

    // First callback is fast (10ms), others are slow (200ms)
    callbacks.push_back(makeSleepCallback(10, 111));
    callbacks.push_back(makeSleepCallback(200, 222));
    callbacks.push_back(makeSleepCallback(200, 333));

    auto result = executeRace(
        callbacks,
        "race_test",
        std::chrono::milliseconds(1000)
    );

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.value.toInt(), 111);  // First one should win
}

TEST_F(FFIAsyncCallbackTest, ExecuteRaceTimeout) {
    std::vector<AsyncCallbackWrapper::CallbackFunc> callbacks;

    // All callbacks are slow
    callbacks.push_back(makeSleepCallback(500, 111));
    callbacks.push_back(makeSleepCallback(500, 222));

    auto result = executeRace(
        callbacks,
        "race_timeout_test",
        std::chrono::milliseconds(100)  // Short timeout
    );

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error_message.find("timeout") != std::string::npos ||
                result.error_message.find("timed out") != std::string::npos);
}

TEST_F(FFIAsyncCallbackTest, ExecuteRaceEmpty) {
    std::vector<AsyncCallbackWrapper::CallbackFunc> callbacks;  // Empty

    auto result = executeRace(callbacks, "race_empty");

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error_message.find("empty") != std::string::npos ||
                result.error_message.find("No callbacks") != std::string::npos);
}

// ============================================================================
// Thread Safety Tests
// ============================================================================

TEST_F(FFIAsyncCallbackTest, ConcurrentExecutions) {
    const int num_threads = 10;
    std::vector<std::thread> threads;
    std::vector<AsyncCallbackResult> results(num_threads);

    // Launch multiple threads executing callbacks
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([i, &results]() {
            AsyncCallbackWrapper wrapper(
                makeSimpleCallback(i),
                fmt::format("thread_{}", i)
            );

            results[i] = wrapper.executeBlocking();
        });
    }

    // Wait for all threads
    for (auto& thread : threads) {
        thread.join();
    }

    // Verify all succeeded
    for (int i = 0; i < num_threads; ++i) {
        EXPECT_TRUE(results[i].success);
        EXPECT_EQ(results[i].value.toInt(), i);
    }
}

TEST_F(FFIAsyncCallbackTest, PoolThreadSafety) {
    AsyncCallbackPool pool(5);

    const int num_threads = 20;
    std::vector<std::thread> threads;

    // Multiple threads submitting to pool
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([i, &pool]() {
            auto future = pool.submit(
                makeSimpleCallback(i),
                fmt::format("pool_thread_{}", i)
            );

            auto result = future.get();
            EXPECT_TRUE(result.success);
        });
    }

    // Wait for all threads
    for (auto& thread : threads) {
        thread.join();
    }
}

// Entry point
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
