// Test program for JavaScript Executor
#include "naab/js_executor.h"
#include "naab/interpreter.h"
#include <fmt/core.h>
#include <iostream>

using namespace naab;

int main() {
    fmt::print("=== JavaScript Executor Test ===\n\n");

    try {
        // Create JavaScript executor
        runtime::JsExecutor executor;

        // Test 1: Execute simple JavaScript code
        fmt::print("1. Executing JavaScript code...\n");
        std::string code = R"(
            function add(a, b) {
                return a + b;
            }

            function multiply(a, b) {
                return a * b;
            }

            function greet(name) {
                return "Hello, " + name + "!";
            }
        )";

        if (!executor.execute(code)) {
            fmt::print("[ERROR] Failed to execute JavaScript\n");
            return 1;
        }

        fmt::print("[SUCCESS] JavaScript code executed\n\n");

        // Test 2: Call add(5, 3)
        fmt::print("2. Test add(5, 3):\n");
        auto result = executor.callFunction("add", {
            std::make_shared<interpreter::Value>(5),
            std::make_shared<interpreter::Value>(3)
        });

        fmt::print("   Result: {}\n", result->toString());
        fmt::print("   Expected: 8\n");

        if (result->toInt() == 8) {
            fmt::print("   ✓ PASS\n\n");
        } else {
            fmt::print("   ✗ FAIL\n\n");
            return 1;
        }

        // Test 3: Call multiply(7, 6)
        fmt::print("3. Test multiply(7, 6):\n");
        result = executor.callFunction("multiply", {
            std::make_shared<interpreter::Value>(7),
            std::make_shared<interpreter::Value>(6)
        });

        fmt::print("   Result: {}\n", result->toString());
        fmt::print("   Expected: 42\n");

        if (result->toInt() == 42) {
            fmt::print("   ✓ PASS\n\n");
        } else {
            fmt::print("   ✗ FAIL\n\n");
            return 1;
        }

        // Test 4: Call greet("NAAb")
        fmt::print("4. Test greet(\"NAAb\"):\n");
        result = executor.callFunction("greet", {
            std::make_shared<interpreter::Value>(std::string("NAAb"))
        });

        fmt::print("   Result: {}\n", result->toString());
        fmt::print("   Expected: Hello, NAAb!\n");

        if (result->toString() == "Hello, NAAb!") {
            fmt::print("   ✓ PASS\n\n");
        } else {
            fmt::print("   ✗ FAIL\n\n");
            return 1;
        }

        // Test 5: Evaluate expression
        fmt::print("5. Test evaluate(\"2 + 2 * 3\"):\n");
        result = executor.evaluate("2 + 2 * 3");

        fmt::print("   Result: {}\n", result->toString());
        fmt::print("   Expected: 8\n");

        if (result->toInt() == 8) {
            fmt::print("   ✓ PASS\n\n");
        } else {
            fmt::print("   ✗ FAIL\n\n");
            return 1;
        }

        fmt::print("=== All Tests Passed! ===\n");

    } catch (const std::exception& e) {
        fmt::print("[ERROR] Exception: {}\n", e.what());
        return 1;
    }

    return 0;
}
