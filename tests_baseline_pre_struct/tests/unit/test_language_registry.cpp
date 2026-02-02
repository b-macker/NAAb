// Test program for Language Registry
#include "naab/language_registry.h"
#include "naab/cpp_executor_adapter.h"
#include "naab/js_executor_adapter.h"
#include "naab/interpreter.h"
#include <fmt/core.h>
#include <iostream>
#include <fstream>
#include <sstream>

using namespace naab;

int main() {
    fmt::print("=== Language Registry Test ===\n\n");

    try {
        // Get registry singleton
        auto& registry = runtime::LanguageRegistry::instance();

        // Test 1: Register C++ executor
        fmt::print("1. Registering C++ executor...\n");
        registry.registerExecutor("cpp", std::make_unique<runtime::CppExecutorAdapter>());
        fmt::print("   ✓ C++ executor registered\n\n");

        // Test 2: Register JavaScript executor
        fmt::print("2. Registering JavaScript executor...\n");
        registry.registerExecutor("javascript", std::make_unique<runtime::JsExecutorAdapter>());
        fmt::print("   ✓ JavaScript executor registered\n\n");

        // Test 3: Check supported languages
        fmt::print("3. Checking supported languages:\n");
        auto languages = registry.supportedLanguages();
        for (const auto& lang : languages) {
            fmt::print("   - {}\n", lang);
        }
        fmt::print("\n");

        // Test 4: Check if languages are supported
        fmt::print("4. Testing isSupported():\n");
        fmt::print("   cpp: {}\n", registry.isSupported("cpp") ? "YES" : "NO");
        fmt::print("   javascript: {}\n", registry.isSupported("javascript") ? "YES" : "NO");
        fmt::print("   python: {}\n", registry.isSupported("python") ? "YES" : "NO");
        fmt::print("\n");

        // Test 5: Get C++ executor and test
        fmt::print("5. Testing C++ executor via registry:\n");
        auto* cpp_executor = registry.getExecutor("cpp");
        if (!cpp_executor) {
            fmt::print("   [ERROR] Failed to get C++ executor\n");
            return 1;
        }

        // Read C++ test code
        std::ifstream cpp_file("../examples/test_cpp_block_add.cpp");
        if (!cpp_file.is_open()) {
            fmt::print("   [ERROR] Could not open test_cpp_block_add.cpp\n");
            return 1;
        }
        std::string cpp_code((std::istreambuf_iterator<char>(cpp_file)),
                              std::istreambuf_iterator<char>());

        // Execute C++ code
        if (!cpp_executor->execute(cpp_code)) {
            fmt::print("   [ERROR] Failed to execute C++ code\n");
            return 1;
        }

        // Call add(5, 3)
        auto result = cpp_executor->callFunction("add", {
            std::make_shared<interpreter::Value>(5),
            std::make_shared<interpreter::Value>(3)
        });

        fmt::print("   C++ add(5, 3) = {}\n", result->toString());
        fmt::print("   Expected: 8\n");

        if (result->toInt() == 8) {
            fmt::print("   ✓ PASS\n\n");
        } else {
            fmt::print("   ✗ FAIL\n\n");
            return 1;
        }

        // Test 6: Get JavaScript executor and test
        fmt::print("6. Testing JavaScript executor via registry:\n");
        auto* js_executor = registry.getExecutor("javascript");
        if (!js_executor) {
            fmt::print("   [ERROR] Failed to get JavaScript executor\n");
            return 1;
        }

        // Execute JavaScript code
        std::string js_code = R"(
            function multiply(a, b) {
                return a * b;
            }
        )";

        if (!js_executor->execute(js_code)) {
            fmt::print("   [ERROR] Failed to execute JavaScript code\n");
            return 1;
        }

        // Call multiply(7, 6)
        result = js_executor->callFunction("multiply", {
            std::make_shared<interpreter::Value>(7),
            std::make_shared<interpreter::Value>(6)
        });

        fmt::print("   JS multiply(7, 6) = {}\n", result->toString());
        fmt::print("   Expected: 42\n");

        if (result->toInt() == 42) {
            fmt::print("   ✓ PASS\n\n");
        } else {
            fmt::print("   ✗ FAIL\n\n");
            return 1;
        }

        // Test 7: Try to get unsupported language
        fmt::print("7. Testing unsupported language:\n");
        auto* python_executor = registry.getExecutor("python");
        if (python_executor == nullptr) {
            fmt::print("   ✓ Correctly returned nullptr for unsupported language\n\n");
        } else {
            fmt::print("   ✗ Should have returned nullptr\n\n");
            return 1;
        }

        fmt::print("=== All Tests Passed! ===\n");

    } catch (const std::exception& e) {
        fmt::print("[ERROR] Exception: {}\n", e.what());
        return 1;
    }

    return 0;
}
