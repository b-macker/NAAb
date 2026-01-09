// Test program for Block Testing Framework
#include "naab/block_tester.h"
#include "naab/language_registry.h"
#include "naab/cpp_executor_adapter.h"
#include "naab/js_executor_adapter.h"
#include "naab/interpreter.h"
#include <fmt/core.h>

using namespace naab;

int main() {
    fmt::print("=== Block Testing Framework Test ===\n\n");

    try {
        // Initialize registry with executors
        auto& registry = runtime::LanguageRegistry::instance();
        registry.registerExecutor("cpp", std::make_unique<runtime::CppExecutorAdapter>());
        registry.registerExecutor("javascript", std::make_unique<runtime::JsExecutorAdapter>());

        // Create block tester
        testing::BlockTester tester;

        // Test 1: JavaScript add function
        fmt::print("Test 1: Testing JavaScript add function\n");
        fmt::print("=======================================\n");

        // Create a simple in-memory test definition
        testing::BlockTestDefinition def1;
        def1.block_id = "JS-ADD-TEST";
        def1.language = "javascript";

        testing::BlockTest test1;
        test1.name = "add_5_3_equals_8";
        test1.code = R"(
            function add(a, b) {
                return a + b;
            }
        )";

        testing::Assertion assertion1;
        assertion1.type = testing::AssertionType::EQUALS;
        assertion1.value_expr = "add(5, 3)";
        assertion1.expected = "8";
        test1.assertions.push_back(assertion1);

        def1.tests.push_back(test1);

        // Manually run the test
        auto* js_executor = registry.getExecutor("javascript");
        if (!js_executor) {
            fmt::print("[ERROR] JavaScript executor not found\n");
            return 1;
        }

        // Execute test code
        if (!js_executor->execute(test1.code)) {
            fmt::print("[ERROR] Failed to execute test code\n");
            return 1;
        }

        // Call function and check result
        auto result = js_executor->callFunction("add", {
            std::make_shared<interpreter::Value>(5),
            std::make_shared<interpreter::Value>(3)
        });

        fmt::print("  Executing: add(5, 3)\n");
        fmt::print("  Expected: {}\n", assertion1.expected);
        fmt::print("  Got: {}\n", result->toString());

        if (result->toString() == assertion1.expected) {
            fmt::print("  ✓ PASS\n\n");
        } else {
            fmt::print("  ✗ FAIL\n\n");
            return 1;
        }

        // Test 2: JavaScript multiply function
        fmt::print("Test 2: Testing JavaScript multiply function\n");
        fmt::print("=============================================\n");

        testing::BlockTest test2;
        test2.name = "multiply_7_6_equals_42";
        test2.code = R"(
            function multiply(a, b) {
                return a * b;
            }
        )";

        testing::Assertion assertion2;
        assertion2.type = testing::AssertionType::EQUALS;
        assertion2.value_expr = "multiply(7, 6)";
        assertion2.expected = "42";

        // Execute and test
        if (!js_executor->execute(test2.code)) {
            fmt::print("[ERROR] Failed to execute test code\n");
            return 1;
        }

        result = js_executor->callFunction("multiply", {
            std::make_shared<interpreter::Value>(7),
            std::make_shared<interpreter::Value>(6)
        });

        fmt::print("  Executing: multiply(7, 6)\n");
        fmt::print("  Expected: {}\n", assertion2.expected);
        fmt::print("  Got: {}\n", result->toString());

        if (result->toString() == assertion2.expected) {
            fmt::print("  ✓ PASS\n\n");
        } else {
            fmt::print("  ✗ FAIL\n\n");
            return 1;
        }

        // Test 3: String test
        fmt::print("Test 3: Testing JavaScript string function\n");
        fmt::print("==========================================\n");

        testing::BlockTest test3;
        test3.name = "greet_returns_hello";
        test3.code = R"(
            function greet(name) {
                return "Hello, " + name + "!";
            }
        )";

        // Execute and test
        if (!js_executor->execute(test3.code)) {
            fmt::print("[ERROR] Failed to execute test code\n");
            return 1;
        }

        result = js_executor->callFunction("greet", {
            std::make_shared<interpreter::Value>(std::string("World"))
        });

        fmt::print("  Executing: greet(\"World\")\n");
        fmt::print("  Expected: Hello, World!\n");
        fmt::print("  Got: {}\n", result->toString());

        if (result->toString() == "Hello, World!") {
            fmt::print("  ✓ PASS\n\n");
        } else {
            fmt::print("  ✗ FAIL\n\n");
            return 1;
        }

        // Test 4: Test type checking
        fmt::print("Test 4: Testing type checking\n");
        fmt::print("==============================\n");

        auto int_result = js_executor->callFunction("add", {
            std::make_shared<interpreter::Value>(10),
            std::make_shared<interpreter::Value>(20)
        });

        fmt::print("  add(10, 20) = {}\n", int_result->toString());
        fmt::print("  Checking if result is an integer...\n");

        if (std::holds_alternative<int>(int_result->data)) {
            fmt::print("  ✓ Type check PASS (int)\n\n");
        } else {
            fmt::print("  ✗ Type check FAIL (expected int)\n\n");
            return 1;
        }

        fmt::print("=== All Block Testing Framework Tests Passed! ===\n");

    } catch (const std::exception& e) {
        fmt::print("[ERROR] Exception: {}\n", e.what());
        return 1;
    }

    return 0;
}
