// Simplified Cross-Language Integration Test
// Tests JavaScript execution without full interpreter dependencies

#include <fmt/core.h>
#include <iostream>
#include <cstdlib>

// QuickJS headers
extern "C" {
#include "quickjs.h"
}

int main() {
    fmt::print("=================================================================\n");
    fmt::print("  NAAb Cross-Language Integration Tests (Simplified)\n");
    fmt::print("=================================================================\n");

    int tests_passed = 0;
    int tests_failed = 0;

    // Test 1: JavaScript Runtime Initialization
    fmt::print("\n[TEST] JavaScript Runtime Initialization\n");
    try {
        JSRuntime* rt = JS_NewRuntime();
        if (!rt) {
            throw std::runtime_error("Failed to create JavaScript runtime");
        }

        JSContext* ctx = JS_NewContext(rt);
        if (!ctx) {
            JS_FreeRuntime(rt);
            throw std::runtime_error("Failed to create JavaScript context");
        }

        fmt::print("  ✓ JavaScript runtime initialized\n");

        // Test 2: JavaScript Code Execution
        fmt::print("\n[TEST] JavaScript Code Execution\n");
        const char* js_code = R"(
            function add(a, b) {
                return a + b;
            }
            add(15, 27);
        )";

        JSValue result = JS_Eval(ctx, js_code, strlen(js_code), "<test>", JS_EVAL_TYPE_GLOBAL);

        if (JS_IsException(result)) {
            throw std::runtime_error("JavaScript execution failed");
        }

        int32_t value = 0;
        if (JS_ToInt32(ctx, &value, result) != 0) {
            JS_FreeValue(ctx, result);
            throw std::runtime_error("Failed to convert result to int");
        }

        if (value != 42) {
            JS_FreeValue(ctx, result);
            throw std::runtime_error(fmt::format("Expected 42, got {}", value));
        }

        fmt::print("  ✓ JavaScript execution: add(15, 27) = {}\n", value);
        JS_FreeValue(ctx, result);

        // Test 3: JavaScript String Operations
        fmt::print("\n[TEST] JavaScript String Operations\n");
        const char* str_code = "'Hello' + ' ' + 'World'";
        result = JS_Eval(ctx, str_code, strlen(str_code), "<test>", JS_EVAL_TYPE_GLOBAL);

        if (JS_IsException(result)) {
            throw std::runtime_error("String evaluation failed");
        }

        const char* str_value = JS_ToCString(ctx, result);
        if (!str_value) {
            JS_FreeValue(ctx, result);
            throw std::runtime_error("Failed to convert result to string");
        }

        std::string expected = "Hello World";
        std::string actual = str_value;

        if (actual != expected) {
            JS_FreeCString(ctx, str_value);
            JS_FreeValue(ctx, result);
            throw std::runtime_error(fmt::format("Expected '{}', got '{}'", expected, actual));
        }

        fmt::print("  ✓ String concatenation: '{}'\n", str_value);
        JS_FreeCString(ctx, str_value);
        JS_FreeValue(ctx, result);

        tests_passed += 3;

        // Cleanup
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);

        fmt::print("\n[PASS] All JavaScript tests passed\n");

    } catch (const std::exception& e) {
        tests_failed++;
        fmt::print("\n[FAIL] Test failed: {}\n", e.what());
    }

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
