# Phase 1 Item 10 Day 5: Polyglot Async Integration

## Overview

Integration of the async callback framework with Python/JavaScript/C++ polyglot executors for thread-safe cross-language async operations.

## Components

### 1. Python Async Executor (`PythonAsyncExecutor`)

**Thread Safety**: Uses thread-local Python executor instances for GIL safety.

**Features**:
- Automatic GIL management via pybind11
- Thread-local executor instances (one per thread)
- Exception handling across Python-C++ boundary
- Timeout protection for Python code execution

**Example**:
```cpp
PythonAsyncExecutor executor;
auto future = executor.executeAsync("2 + 2", {});
auto result = future.get();
// result.success == true
// result.value.toInt() == 4
```

### 2. JavaScript Async Executor (`JavaScriptAsyncExecutor`)

**Thread Safety**: Uses thread-local QuickJS contexts (one per thread).

**Features**:
- Isolated JavaScript contexts per thread
- Exception handling for JavaScript errors
- Timeout protection
- Thread-safe value marshaling

**Example**:
```cpp
JavaScriptAsyncExecutor executor;
auto future = executor.executeAsync("10 * 5", {});
auto result = future.get();
// result.success == true
// result.value.toInt() == 50
```

### 3. C++ Async Executor (`CppAsyncExecutor`)

**Thread Safety**: Compiled code is thread-safe (assuming user code is).

**Features**:
- Dynamic compilation and loading
- Unique block IDs for concurrent compilation
- Timeout protection for compilation + execution
- Type marshaling via shared pointers

**Example**:
```cpp
CppAsyncExecutor executor;
std::string code = R"(
#include <memory>
#include "naab/value.h"

extern "C" std::shared_ptr<naab::interpreter::Value> execute() {
    return std::make_shared<naab::interpreter::Value>(42);
}
)";
auto future = executor.executeAsync(code, {});
auto result = future.get();
// result.success == true
// result.value.toInt() == 42
```

### 4. Unified Polyglot Executor (`PolyglotAsyncExecutor`)

**Features**:
- Language-agnostic interface
- Parallel execution of mixed-language blocks
- Automatic routing to correct executor

**Example**:
```cpp
PolyglotAsyncExecutor executor;

// Execute Python
auto py_future = executor.executeAsync(
    PolyglotAsyncExecutor::Language::Python,
    "10 + 5",
    {}
);

// Execute JavaScript
auto js_future = executor.executeAsync(
    PolyglotAsyncExecutor::Language::JavaScript,
    "20 + 5",
    {}
);

// Execute in parallel
std::vector<std::tuple<Language, std::string, std::vector<Value>>> blocks = {
    {Language::Python, "10 + 5", {}},
    {Language::JavaScript, "20 + 5", {}},
};
auto results = executor.executeParallel(blocks);
```

## Convenience Functions

Quick execution without creating executor instances:

```cpp
// Python
auto py_future = executePythonAsync("2 + 2");

// JavaScript
auto js_future = executeJavaScriptAsync("3 * 3");

// C++
auto cpp_future = executeCppAsync(cpp_code);
```

## Thread Safety Guarantees

### Python
- **GIL Handling**: Automatic via pybind11
- **Isolation**: Thread-local executor instances
- **Safety**: Multiple threads can safely execute Python code concurrently

### JavaScript
- **Context Isolation**: Each thread gets its own QuickJS context
- **Safety**: No shared state between threads
- **Limitation**: JS is single-threaded per context (as expected)

### C++
- **Compilation**: Synchronized via unique block IDs
- **Execution**: User code must be thread-safe
- **Safety**: No shared state in executor

## Exception Handling

All executors properly catch and convert exceptions:

**Python Exceptions** → `AsyncCallbackResult` with error details
**JavaScript Errors** → `AsyncCallbackResult` with error details
**C++ Exceptions** → `AsyncCallbackResult` with error details

**Example**:
```cpp
auto future = executePythonAsync("raise ValueError('Test')");
auto result = future.get();
// result.success == false
// result.error_message contains "ValueError"
```

## Timeout Protection

All polyglot executions have configurable timeouts:

```cpp
// Python with 5 second timeout
auto result = executor.executeBlocking(
    code,
    args,
    std::chrono::milliseconds(5000)
);
```

## Integration with Async Callback Framework

The polyglot async executors build on top of the `AsyncCallbackWrapper` from Day 4:

1. **Wrap polyglot code** in a callback function
2. **Execute via AsyncCallbackWrapper** for timeout/cancellation
3. **Handle exceptions** across language boundaries
4. **Return results** via `AsyncCallbackResult`

## Testing

Comprehensive test suite covers:

- **Basic execution** (Python, JS, C++)
- **Exception handling** across all languages
- **Timeout protection**
- **Concurrent execution** from multiple threads
- **Thread safety** verification
- **Mixed-language parallel execution**

**Test file**: `tests/unit/polyglot_async_test.cpp` (20+ tests)

## Performance Considerations

### Python
- **GIL overhead**: Minimal due to automatic handling
- **Thread-local executors**: No inter-thread contention
- **Best for**: I/O-bound polyglot blocks

### JavaScript
- **Context creation**: One-time cost per thread
- **QuickJS overhead**: Fast and lightweight
- **Best for**: Short-lived computations

### C++
- **Compilation cost**: Higher, but cached
- **Execution**: Native speed
- **Best for**: Performance-critical computations

## API Reference

### PythonAsyncExecutor

```cpp
class PythonAsyncExecutor {
public:
    std::future<AsyncCallbackResult> executeAsync(
        const std::string& code,
        const std::vector<Value>& args,
        std::chrono::milliseconds timeout = 30s
    );

    AsyncCallbackResult executeBlocking(
        const std::string& code,
        const std::vector<Value>& args,
        std::chrono::milliseconds timeout = 30s
    );
};
```

### JavaScriptAsyncExecutor

```cpp
class JavaScriptAsyncExecutor {
public:
    std::future<AsyncCallbackResult> executeAsync(
        const std::string& code,
        const std::vector<Value>& args,
        std::chrono::milliseconds timeout = 30s
    );

    AsyncCallbackResult executeBlocking(
        const std::string& code,
        const std::vector<Value>& args,
        std::chrono::milliseconds timeout = 30s
    );
};
```

### CppAsyncExecutor

```cpp
class CppAsyncExecutor {
public:
    std::future<AsyncCallbackResult> executeAsync(
        const std::string& code,
        const std::vector<Value>& args,
        std::chrono::milliseconds timeout = 60s  // Longer for compilation
    );

    AsyncCallbackResult executeBlocking(
        const std::string& code,
        const std::vector<Value>& args,
        std::chrono::milliseconds timeout = 60s
    );
};
```

### PolyglotAsyncExecutor

```cpp
class PolyglotAsyncExecutor {
public:
    enum class Language { Python, JavaScript, Cpp };

    std::future<AsyncCallbackResult> executeAsync(
        Language language,
        const std::string& code,
        const std::vector<Value>& args,
        std::chrono::milliseconds timeout = 30s
    );

    AsyncCallbackResult executeBlocking(
        Language language,
        const std::string& code,
        const std::vector<Value>& args,
        std::chrono::milliseconds timeout = 30s
    );

    std::vector<AsyncCallbackResult> executeParallel(
        const std::vector<std::tuple<Language, std::string, std::vector<Value>>>& blocks,
        std::chrono::milliseconds timeout = 30s
    );
};
```

## Security Considerations

1. **Timeout Protection**: All executions have timeouts to prevent DoS
2. **Exception Isolation**: Exceptions don't cross FFI boundaries unsafely
3. **Audit Logging**: All polyglot executions are logged for security auditing
4. **Thread Safety**: No data races in executor implementations
5. **GIL Respect**: Python GIL is properly acquired/released

## Future Enhancements (Day 6)

- Performance benchmarks
- Optimization of thread-local executor management
- Enhanced error reporting with stack traces
- Support for passing arguments to polyglot blocks
- Streaming results for long-running polyglot operations

## Files

### Headers
- `include/naab/polyglot_async_executor.h` - API declarations

### Implementation
- `src/runtime/polyglot_async_executor.cpp` - Executor implementations

### Tests
- `tests/unit/polyglot_async_test.cpp` - Comprehensive test suite

### Documentation
- `docs/POLYGLOT_ASYNC_INTEGRATION.md` - This file

## Status

✅ **Complete** - All components implemented and tested

**Next**: Item 10 Day 6 - Performance & Documentation
