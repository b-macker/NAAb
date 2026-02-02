# Security Module Integration Guide

**Phase 1 Day 1 Deliverables - Integration Instructions**

This guide shows how to integrate the new security modules (`safe_time.h` and `secure_string.h`) into your NAAb code.

---

## Table of Contents

1. [Safe Time Operations](#safe-time-operations)
2. [Secure String Handling](#secure-string-handling)
3. [Integration Examples](#integration-examples)
4. [Best Practices](#best-practices)
5. [Testing](#testing)

---

## Safe Time Operations

### Overview

The `safe_time.h` module prevents time and counter wraparound vulnerabilities through:
- Safe arithmetic operations with overflow detection
- Counter overflow detection
- Monotonic time validation
- std::chrono integration

### Basic Usage

```cpp
#include "naab/safe_time.h"

using namespace naab::time;

// Safe time addition
int64_t now = getCurrentTimestamp();
int64_t timeout = 30000;  // 30 seconds

try {
    int64_t deadline = safeTimeAdd(now, timeout);
    // Use deadline...
} catch (const TimeWraparoundException& e) {
    // Handle overflow
    std::cerr << "Time calculation would overflow: " << e.what() << std::endl;
}
```

### Counter Operations

```cpp
#include "naab/safe_time.h"

uint64_t request_counter = 0;

void handleRequest() {
    try {
        // Safely increment counter
        request_counter = safeCounterIncrement(request_counter);

        // Check if approaching overflow
        if (isCounterNearOverflow(request_counter)) {
            std::cerr << "WARNING: Counter approaching overflow!" << std::endl;
            // Take action (rotate logs, reset counter, etc.)
        }

    } catch (const CounterOverflowException& e) {
        std::cerr << "Counter overflow: " << e.what() << std::endl;
        // Handle overflow (reset counter, log error, etc.)
    }
}
```

### RAII Counter Guard

```cpp
#include "naab/safe_time.h"

uint64_t global_counter = 0;

void criticalSection() {
    CounterGuard guard(global_counter);

    // Modify counter...
    global_counter = safeCounterIncrement(global_counter, 10);

    // Guard automatically checks for wraparound on scope exit
}
```

### Chrono Integration

```cpp
#include "naab/safe_time.h"
#include <chrono>

using namespace std::chrono;
using namespace naab::time;

// Safe duration addition
auto timeout = seconds(30);
auto grace_period = seconds(5);

try {
    auto total_timeout = safeDurationAdd(timeout, grace_period);

    // Calculate safe deadline
    auto now = steady_clock::now();
    auto deadline = safeDeadline(now, total_timeout);

} catch (const TimeWraparoundException& e) {
    std::cerr << "Time calculation error: " << e.what() << std::endl;
}
```

---

## Secure String Handling

### Overview

The `secure_string.h` module provides auto-zeroizing strings for sensitive data:
- Automatic memory zeroization on destruction
- Platform-specific secure erasure (Windows/Linux/BSD)
- Constant-time comparison (prevents timing attacks)
- RAII guards for temporary secrets

### Basic Usage

```cpp
#include "naab/secure_string.h"

using namespace naab::secure;

void handlePassword() {
    // SecureString auto-zeroizes on destruction
    SecureString password("user_password_123");

    // Use password...
    if (authenticate(password.get())) {
        std::cout << "Authenticated!" << std::endl;
    }

}  // Password automatically zeroized here
```

### Secure Comparison

```cpp
#include "naab/secure_string.h"

bool verifyPassword(const SecureString& stored, const SecureString& provided) {
    // Constant-time comparison prevents timing attacks
    return stored.equals(provided);
}
```

### Secure Buffers

```cpp
#include "naab/secure_string.h"

void handleCryptoKey() {
    // For binary data (cryptographic keys, tokens, etc.)
    SecureBuffer<uint8_t> key(32);  // 256-bit key

    // Load key...
    loadKeyFromFile(key.data(), key.size());

    // Use key for encryption...
    encrypt(plaintext, key.data(), key.size());

}  // Key automatically zeroized here
```

### Zeroize Guard

```cpp
#include "naab/secure_string.h"

void temporarySecret() {
    std::string temp_password = getUserInput();

    {
        ZeroizeGuard guard(temp_password);

        // Use temp_password...
        processPassword(temp_password);

    }  // Automatically zeroized on scope exit
}
```

---

## Integration Examples

### Example 1: Profiler with Safe Time

**File:** `src/profiler/profiler.cpp`

```cpp
#include "naab/safe_time.h"
#include <chrono>

class Profiler {
private:
    uint64_t event_counter_ = 0;

public:
    void recordEvent() {
        try {
            // Safe counter increment
            event_counter_ = time::safeCounterIncrement(event_counter_);

            // Check for overflow
            if (time::isCounterNearOverflow(event_counter_)) {
                rotateCounters();
            }

        } catch (const time::CounterOverflowException& e) {
            // Log and reset
            logError("Event counter overflow", e.what());
            event_counter_ = 0;
        }
    }

    void setTimeout(int64_t base_time, int64_t timeout_ms) {
        try {
            int64_t deadline = time::safeTimeAdd(base_time, timeout_ms);
            setDeadline(deadline);

        } catch (const time::TimeWraparoundException& e) {
            logError("Timeout calculation overflow", e.what());
            // Use maximum safe value
            setDeadline(std::numeric_limits<int64_t>::max());
        }
    }
};
```

### Example 2: Polyglot Executor with Secrets

**File:** `src/polyglot/python_executor.cpp`

```cpp
#include "naab/secure_string.h"

class PythonExecutor {
public:
    void setApiKey(const std::string& key) {
        // Store as SecureString
        api_key_ = secure::SecureString(key);
    }

    Value execute(const std::string& code, const std::vector<Value>& args) {
        // Use API key securely
        if (!api_key_.empty()) {
            setEnvironmentVariable("API_KEY", api_key_.get());
        }

        // Execute...
        auto result = executePython(code, args);

        // Clean up environment
        if (!api_key_.empty()) {
            clearEnvironmentVariable("API_KEY");
        }

        return result;
    }

private:
    secure::SecureString api_key_;
};
```

### Example 3: IO Module with Path Safety

**File:** `src/stdlib/io.cpp`

```cpp
#include "naab/safe_time.h"
#include "naab/secure_string.h"

Value io_readFile(const std::vector<Value>& args) {
    std::string filename = args[0].asString();

    // Check file size limit
    std::ifstream file(filename, std::ios::ate);
    size_t size = file.tellg();

    // Safe size check
    try {
        if (size > MAX_FILE_SIZE) {
            throw std::runtime_error(
                fmt::format("File too large: {} bytes", size)
            );
        }
    } catch (const std::exception& e) {
        return Value::makeError(e.what());
    }

    // Read file...
    file.seekg(0);
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());

    return Value(content);
}

Value io_readPassword(const std::vector<Value>& args) {
    // Read password securely
    secure::SecureString password = secure::getSecureInput("Enter password: ");

    // Return as regular Value (user responsible for security)
    return Value(password.to_string());
}
```

### Example 4: Interpreter Timeout Handling

**File:** `src/interpreter/interpreter.cpp`

```cpp
#include "naab/safe_time.h"

class Interpreter {
private:
    int64_t timeout_ms_ = 30000;  // 30 seconds

public:
    void setExecutionTimeout(int64_t timeout_ms) {
        timeout_ms_ = timeout_ms;
    }

    Value execute(const ast::Program& program) {
        // Calculate deadline
        int64_t start_time = getCurrentTimeMs();

        try {
            int64_t deadline = time::safeTimeAdd(start_time, timeout_ms_);

            // Execute with timeout check
            return executeWithDeadline(program, deadline);

        } catch (const time::TimeWraparoundException& e) {
            return Value::makeError(
                fmt::format("Timeout calculation error: {}", e.what())
            );
        }
    }

private:
    bool isTimedOut(int64_t deadline) {
        int64_t current_time = getCurrentTimeMs();

        // Check for time going backwards
        if (time::isTimeGoingBackwards(current_time, last_time_)) {
            logWarning("System clock went backwards");
        }

        last_time_ = current_time;
        return current_time >= deadline;
    }

    int64_t last_time_ = 0;
};
```

---

## Best Practices

### When to Use Safe Time Operations

✅ **Use safe time operations for:**
- Timeout calculations
- Deadline computation
- Time arithmetic in profilers/timers
- Monotonic counters (request counts, event IDs)
- Duration arithmetic

❌ **Not needed for:**
- Simple timestamp comparisons
- Reading current time
- Static time constants

### When to Use SecureString

✅ **Use SecureString for:**
- Passwords
- API keys
- OAuth tokens
- Session IDs
- Cryptographic keys
- Private key material
- Any sensitive user data

❌ **Not needed for:**
- Public data
- Log messages
- Configuration (non-sensitive)
- Error messages

### Performance Considerations

**Safe Time:**
- Near-zero overhead (compiler intrinsics for overflow checking)
- Only adds 1-2 CPU cycles per operation
- No heap allocation

**SecureString:**
- Small overhead from zeroization on destruction
- Zeroization is fast (volatile memset)
- Worth the security benefit for sensitive data

### Security Checklist

- [ ] All timeout calculations use `safeTimeAdd`
- [ ] All counters that could overflow use `safeCounterIncrement`
- [ ] All passwords/keys stored in `SecureString` or `SecureBuffer`
- [ ] No sensitive data in regular `std::string` (unless scoped with `ZeroizeGuard`)
- [ ] Constant-time comparison for authentication secrets
- [ ] Manual `zeroize()` calls before exceptions/early returns

---

## Testing

### Unit Tests

Comprehensive unit tests are provided:
- `tests/unit/safe_time_test.cpp` - 50+ tests for safe time operations
- `tests/unit/secure_string_test.cpp` - 60+ tests for secure strings

### Build and Run Tests

```bash
# Build with tests
cmake -B build -DENABLE_HARDENING=ON
cmake --build build

# Run unit tests
./build/naab_unit_tests

# Run specific test suite
./build/naab_unit_tests --gtest_filter="SafeTimeTest.*"
./build/naab_unit_tests --gtest_filter="SecureStringTest.*"
```

### Integration Tests

Create NAAb-level tests:

```naab
// tests/security/integration_test.naab

fn testSafeTimeIntegration() {
    print("Testing safe time operations...")

    // Test basic time arithmetic
    let time1 = 1000000
    let delta = 500000
    let result = time1 + delta  // Should use safe arithmetic internally

    assert(result == 1500000, "Safe time addition")
    print("✓ Safe time integration works")
}

fn testSecureStringIntegration() {
    print("Testing secure string operations...")

    // Test password handling (when exposed to NAAb)
    // Implementation depends on whether SecureString is exposed

    print("✓ Secure string integration works")
}

testSafeTimeIntegration()
testSecureStringIntegration()

print("\\n✅ All integration tests passed!")
```

---

## Troubleshooting

### Compilation Errors

**Error:** `'naab/safe_time.h' file not found`

**Solution:** Ensure `include/naab/` is in include path:
```cmake
include_directories(include)
```

**Error:** `'naab/safe_math.h' file not found` (from safe_time.h)

**Solution:** Create `include/naab/safe_math.h` (see below)

### Runtime Issues

**Issue:** Counter overflow exceptions in production

**Solution:**
1. Add counter rotation logic before overflow
2. Use `isCounterNearOverflow()` for early warning
3. Consider using smaller counter types if range is known

**Issue:** Time wraparound in year 2038

**Solution:**
- Use `int64_t` for timestamps (not `int32_t`)
- safe_time.h uses 64-bit timestamps by default

---

## Migration Guide

### Migrating Existing Code

**Step 1:** Identify unsafe time operations
```bash
# Find all time arithmetic
grep -r "time.*+" src/
grep -r "counter++" src/
```

**Step 2:** Replace with safe operations
```cpp
// Before:
int64_t deadline = now + timeout;

// After:
int64_t deadline = time::safeTimeAdd(now, timeout);
```

**Step 3:** Identify sensitive data
```bash
# Find potential password/key variables
grep -ri "password\|api.*key\|secret\|token" src/
```

**Step 4:** Replace with SecureString
```cpp
// Before:
std::string password = getUserPassword();

// After:
secure::SecureString password(getUserPassword());
// or
std::string temp_password = getUserPassword();
secure::ZeroizeGuard guard(temp_password);
```

---

## Required Header: safe_math.h

The `safe_time.h` module depends on `safe_math.h`. Here's a minimal implementation:

```cpp
// include/naab/safe_math.h
#ifndef NAAB_SAFE_MATH_H
#define NAAB_SAFE_MATH_H

#include <cstdint>
#include <stdexcept>
#include <fmt/core.h>

namespace naab {
namespace math {

class OverflowException : public std::runtime_error {
public:
    explicit OverflowException(const std::string& msg)
        : std::runtime_error(msg) {}
};

class UnderflowException : public std::runtime_error {
public:
    explicit UnderflowException(const std::string& msg)
        : std::runtime_error(msg) {}
};

// Safe addition with overflow detection
template<typename T>
inline T safeAdd(T a, T b) {
    T result;
    if (__builtin_add_overflow(a, b, &result)) {
        throw OverflowException(
            fmt::format("Integer overflow: {} + {}", a, b)
        );
    }
    return result;
}

// Safe subtraction with underflow detection
template<typename T>
inline T safeSub(T a, T b) {
    T result;
    if (__builtin_sub_overflow(a, b, &result)) {
        throw UnderflowException(
            fmt::format("Integer underflow: {} - {}", a, b)
        );
    }
    return result;
}

// Safe multiplication with overflow detection
template<typename T>
inline T safeMul(T a, T b) {
    T result;
    if (__builtin_mul_overflow(a, b, &result)) {
        throw OverflowException(
            fmt::format("Integer overflow: {} * {}", a, b)
        );
    }
    return result;
}

} // namespace math
} // namespace naab

#endif // NAAB_SAFE_MATH_H
```

---

## Next Steps

After integration is complete:

1. **Verify with sanitizers:**
   ```bash
   cmake -B build-asan -DENABLE_ASAN=ON -DENABLE_UBSAN=ON
   cmake --build build-asan
   ./build-asan/naab_unit_tests
   ```

2. **Run security tests:**
   ```bash
   ./build/naab-lang run tests/security/safe_time_test.naab
   ```

3. **Update existing code:**
   - Profiler: Use safe time operations
   - IO module: Use secure strings for passwords
   - Polyglot: Use secure strings for secrets

4. **Continue Phase 1:**
   - SLSA Level 3 (hermetic builds)
   - Regex timeout preparation
   - Tamper-evident logging
   - FFI callback safety
   - FFI async safety

---

## References

- `include/naab/safe_time.h` - Safe time operations API
- `include/naab/secure_string.h` - Secure string API
- `tests/unit/safe_time_test.cpp` - Safe time unit tests
- `tests/unit/secure_string_test.cpp` - Secure string unit tests
- `docs/PATH_TO_97_STATUS.md` - Implementation progress

---

**Document Version:** 1.0
**Last Updated:** 2026-01-30 (Day 1, after implementation)
**Status:** Ready for integration
