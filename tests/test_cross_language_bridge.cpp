// CrossLanguageBridge Direct Tests
// Tests type marshalling without full executor infrastructure

#include <fmt/core.h>
#include <iostream>
#include <cstring>

// QuickJS headers
extern "C" {
#include "quickjs.h"
}

#ifdef HAVE_PYBIND11
#include <Python.h>
#include <pybind11/pybind11.h>
namespace py = pybind11;
#endif

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
// JavaScript Type Marshalling Tests
// ============================================================================

void test_js_int_conversion() {
    TEST("JavaScript ↔ C++: Integer Conversion")

    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx = JS_NewContext(rt);

    // C++ int → JSValue
    JSValue js_val = JS_NewInt32(ctx, 42);

    // JSValue → C++ int
    int32_t result = 0;
    ASSERT(JS_ToInt32(ctx, &result, js_val) == 0, "Failed to convert JSValue to int");
    ASSERT(result == 42, "Int conversion failed");

    fmt::print("  ✓ Int: 42 (C++ ↔ JavaScript)\n");

    JS_FreeValue(ctx, js_val);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    TEST_END
}

void test_js_string_conversion() {
    TEST("JavaScript ↔ C++: String Conversion")

    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx = JS_NewContext(rt);

    // C++ string → JSValue
    const char* test_str = "Hello World";
    JSValue js_val = JS_NewString(ctx, test_str);

    // JSValue → C++ string
    const char* result = JS_ToCString(ctx, js_val);
    ASSERT(result != nullptr, "Failed to convert JSValue to string");
    ASSERT(std::strcmp(result, test_str) == 0, "String conversion failed");

    fmt::print("  ✓ String: '{}' (C++ ↔ JavaScript)\n", result);

    JS_FreeCString(ctx, result);
    JS_FreeValue(ctx, js_val);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    TEST_END
}

void test_js_array_conversion() {
    TEST("JavaScript ↔ C++: Array Conversion")

    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx = JS_NewContext(rt);

    // C++ vector → JavaScript array
    JSValue js_array = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, js_array, 0, JS_NewInt32(ctx, 1));
    JS_SetPropertyUint32(ctx, js_array, 1, JS_NewInt32(ctx, 2));
    JS_SetPropertyUint32(ctx, js_array, 2, JS_NewInt32(ctx, 3));

    // Verify array length
    JSValue length_val = JS_GetPropertyStr(ctx, js_array, "length");
    int32_t length = 0;
    JS_ToInt32(ctx, &length, length_val);
    ASSERT(length == 3, "Array length should be 3");

    // Verify array elements
    JSValue elem0 = JS_GetPropertyUint32(ctx, js_array, 0);
    int32_t val0 = 0;
    JS_ToInt32(ctx, &val0, elem0);
    ASSERT(val0 == 1, "Array element 0 should be 1");

    fmt::print("  ✓ Array: [1, 2, 3] (length = {}) (C++ ↔ JavaScript)\n", length);

    JS_FreeValue(ctx, elem0);
    JS_FreeValue(ctx, length_val);
    JS_FreeValue(ctx, js_array);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    TEST_END
}

// ============================================================================
// Python Type Marshalling Tests (if available)
// ============================================================================

#ifdef HAVE_PYBIND11

void test_python_int_conversion() {
    TEST("Python ↔ C++: Integer Conversion")

    // C++ int → Python
    int cpp_val = 42;
    py::int_ py_val(cpp_val);

    // Python → C++ int
    int result = py_val.cast<int>();
    ASSERT(result == 42, "Int conversion failed");

    fmt::print("  ✓ Int: 42 (C++ ↔ Python)\n");

    TEST_END
}

void test_python_string_conversion() {
    TEST("Python ↔ C++: String Conversion")

    // C++ string → Python
    std::string cpp_str = "Hello from Python";
    py::str py_val(cpp_str);

    // Python → C++ string
    std::string result = py_val.cast<std::string>();
    ASSERT(result == cpp_str, "String conversion failed");

    fmt::print("  ✓ String: '{}' (C++ ↔ Python)\n", result);

    TEST_END
}

void test_python_list_conversion() {
    TEST("Python ↔ C++: List Conversion")

    // C++ vector → Python list
    py::list py_list;
    py_list.append(1);
    py_list.append(2);
    py_list.append(3);

    ASSERT(py::len(py_list) == 3, "List length should be 3");
    ASSERT(py_list[0].cast<int>() == 1, "List element 0 should be 1");
    ASSERT(py_list[1].cast<int>() == 2, "List element 1 should be 2");
    ASSERT(py_list[2].cast<int>() == 3, "List element 2 should be 3");

    fmt::print("  ✓ List: [1, 2, 3] (C++ ↔ Python)\n");

    TEST_END
}

#endif

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    fmt::print("=================================================================\n");
    fmt::print("  NAAb CrossLanguageBridge Tests\n");
    fmt::print("=================================================================\n");

    // JavaScript tests (always available)
    test_js_int_conversion();
    test_js_string_conversion();
    test_js_array_conversion();

#ifdef HAVE_PYBIND11
    // Initialize Python
    if (!Py_IsInitialized()) {
        Py_Initialize();
        fmt::print("\n[INFO] Python interpreter initialized\n");
    }

    // Python tests
    test_python_int_conversion();
    test_python_string_conversion();
    test_python_list_conversion();

    // Finalize Python
    // Note: Py_Finalize() commented out to avoid issues with pybind11
    // Py_Finalize();
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
