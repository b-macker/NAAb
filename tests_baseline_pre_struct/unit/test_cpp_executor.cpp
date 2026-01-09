// Test program for C++ Executor with type marshalling
#include "naab/cpp_executor.h"
#include "naab/interpreter.h"
#include <fmt/core.h>
#include <iostream>
#include <fstream>

using namespace naab;

int main() {
    fmt::print("=== C++ Executor Test ===\n\n");

    try {
        // Create executor
        runtime::CppExecutor executor;

        // Read the test C++ code from examples/test_cpp_block_add.cpp
        std::ifstream code_file("../examples/test_cpp_block_add.cpp");
        if (!code_file.is_open()) {
            fmt::print("[ERROR] Failed to open test C++ file\n");
            return 1;
        }

        std::string test_code((std::istreambuf_iterator<char>(code_file)),
                               std::istreambuf_iterator<char>());
        code_file.close();

        // Compile block
        fmt::print("1. Compiling C++ block from examples/test_cpp_block_add.cpp...\n");
        bool compiled = executor.compileBlock("TEST-MATH-001", test_code, "add");

        if (!compiled) {
            fmt::print("[ERROR] Failed to compile block\n");
            return 1;
        }

        fmt::print("[SUCCESS] Block compiled\n\n");

        // Test 1: Call add(5, 3)
        fmt::print("2. Test add(5, 3):\n");
        auto arg1 = std::make_shared<interpreter::Value>(5);
        auto arg2 = std::make_shared<interpreter::Value>(3);
        std::vector<std::shared_ptr<interpreter::Value>> args = {arg1, arg2};

        auto result = executor.callFunction("TEST-MATH-001", "add", args);

        fmt::print("   Result: {}\n", result->toString());
        fmt::print("   Expected: 8\n");

        if (result->toInt() == 8) {
            fmt::print("   ✓ PASS\n\n");
        } else {
            fmt::print("   ✗ FAIL\n\n");
            return 1;
        }

        // Test 2: Call multiply(7, 6)
        fmt::print("3. Test multiply(7, 6):\n");
        auto arg3 = std::make_shared<interpreter::Value>(7);
        auto arg4 = std::make_shared<interpreter::Value>(6);
        args = {arg3, arg4};

        result = executor.callFunction("TEST-MATH-001", "multiply", args);

        fmt::print("   Result: {}\n", result->toString());
        fmt::print("   Expected: 42\n");

        if (result->toInt() == 42) {
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
