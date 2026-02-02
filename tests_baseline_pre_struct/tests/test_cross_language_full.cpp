// Full Cross-Language Integration Tests
// Tests Python ↔ C++ ↔ JavaScript using actual executor infrastructure

#include <fmt/core.h>
#include <iostream>
#include <cstdlib>
#include <Python.h>

// Minimal Value type matching interpreter::Value structure
#include <memory>
#include <string>
#include <vector>
#include <variant>
#include <unordered_map>

namespace test_value {
    struct Value;

    using ValueData = std::variant<
        std::monostate,
        int,
        double,
        bool,
        std::string,
        std::vector<std::shared_ptr<Value>>,
        std::unordered_map<std::string, std::shared_ptr<Value>>
    >;

    struct Value {
        ValueData data;

        Value() : data(std::monostate{}) {}
        explicit Value(int v) : data(v) {}
        explicit Value(double v) : data(v) {}
        explicit Value(bool v) : data(v) {}
        explicit Value(std::string v) : data(std::move(v)) {}
        explicit Value(std::vector<std::shared_ptr<Value>> v) : data(std::move(v)) {}
        explicit Value(std::unordered_map<std::string, std::shared_ptr<Value>> v) : data(std::move(v)) {}
    };
}

// Make test_value::Value available as interpreter::Value for compatibility
namespace naab {
namespace interpreter {
    using Value = test_value::Value;
}
}

// Now include executor headers
#include "naab/js_executor.h"

#ifdef HAVE_PYBIND11
#include "naab/cross_language_bridge.h"
#include <pybind11/pybind11.h>
namespace py = pybind11;
#endif

using namespace naab::runtime;
using namespace naab::interpreter;

// Test utilities
int tests_passed = 0;
int tests_failed = 0;

#define TEST(name) \
    fmt::print("\n[TEST] {}\n", name); \
    try {

#define TEST_END \
        tests_passed++; \
        fmt::print("[PASS] Test passed\n"); \
    } catch (const std::exception& e) { \
        tests_failed++; \
        fmt::print("[FAIL] Test failed: {}\n", e.what()); \
    }

#define ASSERT(condition, message) \
    if (!(condition)) { \
        throw std::runtime_error(message); \
    }

// ============================================================================
// Test 1: C++ → JavaScript Function Call
// ============================================================================

void test_cpp_to_js_call() {
    TEST("C++ → JavaScript: Function Call")

    JsExecutor js_exec;

    // Define JavaScript function
    const char* js_code = R"(
        function add(a, b) {
            return a + b;
        }

        function greet(name) {
            return "Hello, " + name + "!";
        }
    )";

    bool success = js_exec.execute(js_code);
    ASSERT(success, "Failed to execute JavaScript code");
    fmt::print("  ✓ JavaScript functions defined\n");

    // Test add(15, 27)
    std::vector<std::shared_ptr<Value>> args;
    args.push_back(std::make_shared<Value>(15));
    args.push_back(std::make_shared<Value>(27));

    auto result = js_exec.callFunction("add", args);
    ASSERT(std::get<int>(result->data) == 42, "add(15, 27) should return 42");
    fmt::print("  ✓ add(15, 27) = 42\n");

    // Test greet("NAAb")
    args.clear();
    args.push_back(std::make_shared<Value>(std::string("NAAb")));

    result = js_exec.callFunction("greet", args);
    ASSERT(std::get<std::string>(result->data) == "Hello, NAAb!",
           "greet('NAAb') failed");
    fmt::print("  ✓ greet('NAAb') = 'Hello, NAAb!'\n");

    TEST_END
}

// ============================================================================
// Test 2: JavaScript Expression Evaluation
// ============================================================================

void test_js_evaluation() {
    TEST("JavaScript: Expression Evaluation")

    JsExecutor js_exec;

    // Test number expression
    auto result = js_exec.evaluate("10 + 20 * 2");
    ASSERT(std::get<int>(result->data) == 50, "10 + 20 * 2 should be 50");
    fmt::print("  ✓ 10 + 20 * 2 = 50\n");

    // Test string expression
    result = js_exec.evaluate("'Hello' + ' ' + 'World'");
    ASSERT(std::get<std::string>(result->data) == "Hello World",
           "String concatenation failed");
    fmt::print("  ✓ 'Hello' + ' ' + 'World' = 'Hello World'\n");

    // Test boolean expression
    result = js_exec.evaluate("true && false");
    ASSERT(std::get<bool>(result->data) == false, "true && false should be false");
    fmt::print("  ✓ true && false = false\n");

    TEST_END
}

// ============================================================================
// Python Tests (if available)
// ============================================================================

#ifdef HAVE_PYBIND11

void test_python_cpp_types() {
    TEST("Python ↔ C++: Basic Types")

    CrossLanguageBridge bridge;

    // Test int
    py::int_ py_int(42);
    auto cpp_val = bridge.pythonToValue(py_int);
    ASSERT(std::get<int>(cpp_val->data) == 42, "Python int conversion failed");
    auto back_py = bridge.valueToPython(cpp_val);
    ASSERT(back_py.cast<int>() == 42, "C++ → Python int failed");
    fmt::print("  ✓ Int: 42 (Python ↔ C++)\n");

    // Test string
    py::str py_str("Hello from Python");
    cpp_val = bridge.pythonToValue(py_str);
    ASSERT(std::get<std::string>(cpp_val->data) == "Hello from Python",
           "Python string conversion failed");
    back_py = bridge.valueToPython(cpp_val);
    ASSERT(back_py.cast<std::string>() == "Hello from Python",
           "C++ → Python string failed");
    fmt::print("  ✓ String: 'Hello from Python' (Python ↔ C++)\n");

    TEST_END
}

#endif

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    fmt::print("=================================================================\n");
    fmt::print("  NAAb Cross-Language Integration Tests (Full)\n");
    fmt::print("=================================================================\n");

    // Initialize Python if available
#ifdef HAVE_PYBIND11
    if (!Py_IsInitialized()) {
        Py_Initialize();
        fmt::print("[INFO] Python interpreter initialized\n");
    }
#endif

    // Always run C++ ↔ JavaScript tests
    test_cpp_to_js_call();
    test_js_evaluation();

#ifdef HAVE_PYBIND11
    // Run Python tests only if pybind11 is available
    try {
        py::scoped_interpreter guard; // Initialize pybind11
        test_python_cpp_types();
    } catch (const std::exception& e) {
        fmt::print("[WARN] Python tests failed: {}\n", e.what());
        tests_failed++;
    }
#else
    fmt::print("\n[SKIP] Python tests (pybind11 not available)\n");
#endif

    // Summary
    fmt::print("\n=================================================================\n");
    fmt::print("  Test Summary\n");
    fmt::print("=================================================================\n");
    fmt::print("Passed: {}\n", tests_passed);
    fmt::print("Failed: {}\n", tests_failed);
    fmt::print("Total:  {}\n", tests_passed + tests_failed);

    if (tests_failed == 0) {
        fmt::print("\n✓ ALL TESTS PASSED\n");
        return 0;
    } else {
        fmt::print("\n✗ SOME TESTS FAILED\n");
        return 1;
    }
}
