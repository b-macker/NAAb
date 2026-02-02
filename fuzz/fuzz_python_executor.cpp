// Fuzzer for Python Executor
// Tests Python polyglot block execution and boundary crossing
// Week 2, Task 2.2: FFI/Polyglot Boundary Fuzzing

#include "naab/python_executor.h"
#include "naab/interpreter.h"
#include <cstdint>
#include <cstddef>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Skip empty inputs
    if (size == 0) {
        return 0;
    }

    // Skip very large inputs (polyglot block size limit is 1MB)
    if (size > 50000) {
        return 0;
    }

    // Convert to string (will be Python code)
    std::string code(reinterpret_cast<const char*>(data), size);

    try {
        // Create Python executor
        naab::runtime::PythonExecutor executor;

        // Try to execute the fuzzer-generated Python code
        // This tests:
        // - Python parsing/execution
        // - Error handling
        // - Size limit enforcement
        // - Memory safety in FFI boundary
        executor.execute(code);

        // Successfully executed - good!
        // Fuzzer is looking for crashes, hangs, or sanitizer violations
    } catch (const std::exception& e) {
        // Expected - many inputs will be invalid Python code
        // or trigger size limits, timeouts, etc.
    } catch (...) {
        // Catch any other exceptions
    }

    return 0;
}
