# Phase 1 Items 9 & 10: FFI Safety Implementation Plan

**Date:** 2026-02-01
**Timeline:** 6 days (3 days each)
**Goal:** Complete Phase 1 (reach 95% safety score)

---

## Item 9: FFI Callback Safety (3 days)

### Overview
Ensure callbacks from foreign code (Python, JavaScript, C++) are validated and safe.

### Current State
- FFI callbacks exist in polyglot executors
- No validation of callback signatures
- No type checking at FFI boundary
- No pointer validation
- Exceptions can cross FFI boundaries (unsafe)

### Implementation Goals

#### Day 1: Callback Validation Framework

**Files to Create:**
1. `include/naab/ffi_callback_validator.h` - Callback validation API
2. `src/runtime/ffi_callback_validator.cpp` - Implementation

**Features:**
```cpp
namespace naab {
namespace ffi {

// Callback signature validator
class CallbackValidator {
public:
    // Validate callback signature matches expected types
    static bool validateSignature(
        const std::vector<Value>& args,
        const std::vector<Type>& expected_types
    );

    // Validate callback pointer is safe
    static bool validatePointer(void* callback_ptr);

    // Wrap callback with exception boundary
    template<typename Ret, typename... Args>
    static std::function<Ret(Args...)> wrapCallback(
        std::function<Ret(Args...)> callback
    );
};

} // namespace ffi
} // namespace naab
```

**Safety Checks:**
1. Type validation - ensure arguments match expected types
2. Null pointer checks - reject null callbacks
3. Exception boundaries - catch and convert exceptions
4. Return value validation - check return types

#### Day 2: Integration with Polyglot Executors

**Files to Modify:**
1. `src/polyglot/python_executor.cpp`
2. `src/polyglot/javascript_executor.cpp`
3. `src/polyglot/cpp_executor.cpp`

**Integration Points:**
```cpp
// Before (unsafe):
py::object callback = py::cast(callback_func);
auto result = callback(args...);

// After (safe):
if (!CallbackValidator::validatePointer(callback_func.ptr())) {
    throw FFIException("Invalid callback pointer");
}

auto safe_callback = CallbackValidator::wrapCallback(
    [callback](auto... args) {
        return callback(args...);
    }
);

auto result = safe_callback(args...);
```

**Safety Features:**
- Validate all callback pointers before invocation
- Check argument types match signature
- Wrap with try-catch to prevent exception propagation
- Log validation failures to audit log

#### Day 3: Testing & Documentation

**Tests to Create:**
1. `tests/unit/ffi_callback_validator_test.cpp` - Unit tests
2. `tests/integration/ffi_callback_safety_test.naab` - Integration tests

**Test Cases:**
- ✅ Valid callbacks work correctly
- ✅ Invalid callbacks are rejected
- ✅ Null pointers are caught
- ✅ Type mismatches detected
- ✅ Exceptions don't cross boundaries
- ✅ Return value validation works

**Documentation:**
- `docs/FFI_CALLBACK_SAFETY.md` - Implementation guide
- Update `docs/SECURITY.md` with FFI safety features

---

## Item 10: FFI Async Safety (3 days)

### Overview
Ensure async operations across FFI boundaries are thread-safe and race-free.

### Current State
- Some polyglot operations may be async
- No synchronization for concurrent callbacks
- Potential race conditions in callback state
- No lifetime management for async callbacks

### Implementation Goals

#### Day 1: Async Callback Framework

**Files to Create:**
1. `include/naab/ffi_async_callback.h` - Async callback wrapper
2. `src/runtime/ffi_async_callback.cpp` - Implementation

**Features:**
```cpp
namespace naab {
namespace ffi {

// Thread-safe async callback wrapper
class AsyncCallbackWrapper {
public:
    // Create wrapper with thread synchronization
    AsyncCallbackWrapper(std::function<Value()> callback);

    // Invoke callback (thread-safe)
    Value invoke();

    // Check if callback is still valid (lifetime management)
    bool isValid() const;

    // Cancel pending async operations
    void cancel();

private:
    std::function<Value()> callback_;
    std::mutex mutex_;
    std::atomic<bool> is_valid_{true};
    std::atomic<bool> is_cancelled_{false};
};

// Async result future
class AsyncResult {
public:
    // Wait for result with timeout
    Value wait(std::chrono::milliseconds timeout);

    // Check if ready (non-blocking)
    bool isReady() const;

    // Cancel operation
    void cancel();
};

} // namespace ffi
} // namespace naab
```

**Safety Features:**
1. Mutex protection for callback state
2. Atomic flags for cancellation
3. Timeout support for async waits
4. RAII lifetime management

#### Day 2: Integration with Async Operations

**Files to Modify:**
1. `src/polyglot/python_executor.cpp` - Add async support
2. `src/polyglot/javascript_executor.cpp` - Add async support
3. `include/naab/value.h` - Add AsyncResult type

**Integration Pattern:**
```cpp
// Python async example
py::object async_func = module.attr("async_operation");

// Wrap with thread safety
auto wrapper = std::make_shared<AsyncCallbackWrapper>(
    [async_func]() -> Value {
        auto result = async_func();
        return pyObjectToValue(result);
    }
);

// Return future
auto future = std::async(std::launch::async, [wrapper]() {
    return wrapper->invoke();
});

return Value(AsyncResult(std::move(future)));
```

**Synchronization:**
- Mutex per async operation
- Atomic state tracking
- Proper cleanup on cancel/timeout
- No data races in callback state

#### Day 3: Testing, Documentation & Edge Cases

**Tests to Create:**
1. `tests/unit/ffi_async_callback_test.cpp` - Unit tests
2. `tests/integration/ffi_async_safety_test.naab` - Integration tests
3. `tests/stress/ffi_concurrent_callbacks_test.naab` - Stress tests

**Test Cases:**
- ✅ Concurrent async callbacks work
- ✅ No race conditions in state
- ✅ Cancellation works correctly
- ✅ Timeouts are respected
- ✅ Cleanup happens properly
- ✅ Thread sanitizer passes (TSan)

**Edge Cases to Handle:**
- Callback invoked after cancellation → return error
- Timeout during callback → terminate gracefully
- Callback throws during async → catch and convert
- Multiple concurrent callbacks → proper synchronization
- Callback holds references → lifetime management

**Documentation:**
- `docs/FFI_ASYNC_SAFETY.md` - Async callback guide
- `docs/FFI_BEST_PRACTICES.md` - FFI safety patterns
- Update `README.md` with FFI safety features

---

## Combined Implementation Strategy

### Overall Architecture

```
┌─────────────────────────────────────────────────┐
│          NAAb Interpreter                       │
│                                                 │
│  ┌───────────────────────────────────────────┐ │
│  │   Polyglot Executors                      │ │
│  │   (Python, JS, C++)                       │ │
│  │                                            │ │
│  │   ┌────────────────────────────────────┐  │ │
│  │   │  CallbackValidator                 │  │ │
│  │   │  - Type checking                   │  │ │
│  │   │  - Pointer validation              │  │ │
│  │   │  - Exception boundaries            │  │ │
│  │   └────────────────────────────────────┘  │ │
│  │                                            │ │
│  │   ┌────────────────────────────────────┐  │ │
│  │   │  AsyncCallbackWrapper              │  │ │
│  │   │  - Thread synchronization          │  │ │
│  │   │  - Lifetime management             │  │ │
│  │   │  - Cancellation support            │  │ │
│  │   └────────────────────────────────────┘  │ │
│  └───────────────────────────────────────────┘ │
│                                                 │
│  FFI Boundary (Validated & Thread-Safe)        │
└─────────────────────────────────────────────────┘
           ↕                    ↕
┌──────────────────┐  ┌──────────────────┐
│ Python Runtime   │  │ JavaScript (V8)  │
│ - Callbacks      │  │ - Callbacks      │
│ - Async ops      │  │ - Promises       │
└──────────────────┘  └──────────────────┘
```

### Safety Guarantees

**After Items 9 & 10:**
- ✅ All FFI callbacks validated before invocation
- ✅ No null pointer dereferences at FFI boundary
- ✅ Type mismatches caught and reported
- ✅ Exceptions don't propagate across FFI
- ✅ Async operations are thread-safe
- ✅ No race conditions in callback state
- ✅ Proper lifetime management for async callbacks
- ✅ Cancellation and timeouts supported

---

## Testing Strategy

### Unit Tests (C++)

**Callback Validation Tests:**
```cpp
TEST(CallbackValidatorTest, RejectsNullPointer) {
    EXPECT_FALSE(CallbackValidator::validatePointer(nullptr));
}

TEST(CallbackValidatorTest, ValidatesArgumentTypes) {
    std::vector<Value> args = {Value(42), Value("test")};
    std::vector<Type> expected = {Type::Int, Type::String};
    EXPECT_TRUE(CallbackValidator::validateSignature(args, expected));
}

TEST(CallbackValidatorTest, CatchesExceptions) {
    auto throwing_callback = []() -> Value {
        throw std::runtime_error("test error");
    };
    auto wrapped = CallbackValidator::wrapCallback(throwing_callback);
    EXPECT_NO_THROW(wrapped());  // Exception caught internally
}
```

**Async Safety Tests:**
```cpp
TEST(AsyncCallbackTest, ThreadSafe) {
    auto callback = std::make_shared<AsyncCallbackWrapper>(
        []() { return Value(42); }
    );

    // Launch multiple threads invoking same callback
    std::vector<std::thread> threads;
    for (int i = 0; i < 10; i++) {
        threads.emplace_back([callback]() {
            for (int j = 0; j < 100; j++) {
                callback->invoke();
            }
        });
    }

    for (auto& t : threads) t.join();
    // No crashes = thread-safe
}

TEST(AsyncCallbackTest, CancellationWorks) {
    auto callback = std::make_shared<AsyncCallbackWrapper>(
        []() {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            return Value(42);
        }
    );

    auto future = std::async([callback]() {
        return callback->invoke();
    });

    callback->cancel();
    // Should return quickly without waiting 10 seconds
}
```

### Integration Tests (NAAb)

**Callback Safety Test:**
```naab
fn testCallbackSafety() {
    // Test Python callback
    use python {
        def callback(x: int) -> int:
            return x * 2

        result = callback(21)  # Should work
        assert(result == 42)
    }

    // Test invalid callback (should be caught)
    use python {
        def bad_callback():
            raise ValueError("error")

        try {
            bad_callback()  # Should catch exception
            assert(false, "Should have thrown")
        } catch (e: Error) {
            // Exception caught at FFI boundary
            assert(true)
        }
    }
}
```

**Async Safety Test:**
```naab
fn testAsyncSafety() {
    use python {
        import asyncio

        async def async_operation():
            await asyncio.sleep(0.1)
            return 42

        # Should work with async
        result = asyncio.run(async_operation())
        assert(result == 42)
    }

    // Test concurrent callbacks
    let futures = [];
    for (i in 1..10) {
        use python {
            async def concurrent_op(x: int):
                await asyncio.sleep(0.01)
                return x * 2

            futures.push(asyncio.create_task(concurrent_op(i)))
        }
    }

    // Wait for all (should be thread-safe)
    for (f in futures) {
        use python {
            result = await f
        }
    }
}
```

### Stress Tests

**Concurrent Callback Stress:**
```naab
fn stressConcurrentCallbacks() {
    const NUM_THREADS = 100;
    const CALLS_PER_THREAD = 1000;

    let threads = [];
    for (i in 1..NUM_THREADS) {
        threads.push(async fn() {
            for (j in 1..CALLS_PER_THREAD) {
                use python {
                    def callback(x):
                        return x + 1

                    result = callback(j)
                }
            }
        });
    }

    // Wait for all threads
    for (t in threads) {
        await t;
    }

    // If we get here, no crashes or race conditions
    assert(true)
}
```

---

## Build & Test Commands

### Build with FFI Safety

```bash
cd ~/.naab/language/build

# Rebuild with TSan (thread sanitizer) for async testing
cmake -B build-tsan \
    -DENABLE_TSAN=ON \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_COMPILER=clang++

cmake --build build-tsan -j4
```

### Run Tests

```bash
# Unit tests
./build/naab_unit_tests --gtest_filter=CallbackValidator*
./build/naab_unit_tests --gtest_filter=AsyncCallback*

# Integration tests
./build/naab-lang run tests/integration/ffi_callback_safety_test.naab
./build/naab-lang run tests/integration/ffi_async_safety_test.naab

# Stress tests with TSan
./build-tsan/naab-lang run tests/stress/ffi_concurrent_callbacks_test.naab

# Should report no data races
```

---

## Success Criteria

### Item 9: FFI Callback Safety ✅

- ✅ CallbackValidator implemented and tested
- ✅ All polyglot executors use validation
- ✅ Null pointers rejected
- ✅ Type mismatches detected
- ✅ Exceptions don't cross FFI boundaries
- ✅ 100% test coverage for validation logic
- ✅ Documentation complete

### Item 10: FFI Async Safety ✅

- ✅ AsyncCallbackWrapper implemented
- ✅ Thread synchronization working
- ✅ No race conditions (TSan clean)
- ✅ Cancellation and timeouts work
- ✅ Lifetime management correct
- ✅ Stress tests pass (100+ concurrent callbacks)
- ✅ Documentation complete

### Phase 1 Complete ✅

- ✅ All 10 items implemented
- ✅ Safety score: **95%** (target reached)
- ✅ All tests passing
- ✅ Ready for Phase 2

---

## Timeline Summary

| Day | Item | Tasks | Deliverables |
|-----|------|-------|--------------|
| 1 | Item 9 | Callback validator framework | ffi_callback_validator.h/cpp |
| 2 | Item 9 | Polyglot integration | Updated executors |
| 3 | Item 9 | Testing & docs | Tests + docs |
| 4 | Item 10 | Async framework | ffi_async_callback.h/cpp |
| 5 | Item 10 | Async integration | Updated executors |
| 6 | Item 10 | Testing & docs | Tests + docs + Phase 1 complete |

**Total:** 6 days to complete Phase 1

---

## Risk Mitigation

### Potential Issues

1. **Python GIL (Global Interpreter Lock)**
   - Risk: May cause issues with async Python callbacks
   - Mitigation: Use py::call_guard<py::gil_scoped_release> for non-Python code

2. **JavaScript V8 Threading**
   - Risk: V8 is not thread-safe
   - Mitigation: Run JS callbacks on V8 thread only, use message passing

3. **Performance Overhead**
   - Risk: Validation may slow down callbacks
   - Mitigation: Profile and optimize hot paths, consider caching

### Fallback Plans

- If TSan finds issues: Fix immediately, delay if needed
- If performance unacceptable: Make validation optional (debug builds only)
- If integration complex: Start with Python only, expand later

---

## Expected Outcome

**After 6 days:**
- ✅ Phase 1 complete (10/10 items)
- ✅ Safety score: 95% (A grade)
- ✅ Production-ready FFI safety
- ✅ Comprehensive test coverage
- ✅ Full documentation
- ✅ Ready for Phase 2 or production deployment

**Safety Improvement:**
- Current: 92.5% → Target: 95%
- FFI category: 100% → 100% (already complete, now hardened)
- Concurrency: 31% → 45% (async safety)

---

**Status:** Ready to implement
**Next:** Start with Item 9 Day 1 (Callback Validation Framework)
**ETA:** 6 days to Phase 1 completion

🚀 Let's finish Phase 1 strong! 🚀
