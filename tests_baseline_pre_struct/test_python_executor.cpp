// Quick test for Python executor
#include "naab/python_executor.h"
#include "naab/interpreter.h"
#include <fmt/core.h>
#include <iostream>

using namespace naab::runtime;

int main() {
    fmt::print("=== Python Executor Test ===\n\n");

    try {
        PythonExecutor py_exec;

        // Test 1: Execute simple Python code
        fmt::print("Test 1: Execute Python code\n");
        py_exec.execute("x = 10 + 20");
        fmt::print("   ✓ PASS\n\n");

        // Test 2: Evaluate expression and get result
        fmt::print("Test 2: Evaluate expression\n");
        auto result = py_exec.executeWithResult("x * 2");
        fmt::print("   Result: ");
        std::visit([](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, int>) {
                fmt::print("{} (int)\n", arg);
            } else if constexpr (std::is_same_v<T, double>) {
                fmt::print("{} (double)\n", arg);
            } else {
                fmt::print("unknown type\n");
            }
        }, result->data);
        fmt::print("   ✓ PASS\n\n");

        // Test 3: Define and call Python function
        fmt::print("Test 3: Define and call Python function\n");
        py_exec.execute(R"(
def add(a, b):
    return a + b
)");

        std::vector<std::shared_ptr<naab::interpreter::Value>> args;
        args.push_back(std::make_shared<naab::interpreter::Value>(15));
        args.push_back(std::make_shared<naab::interpreter::Value>(25));

        auto func_result = py_exec.callFunction("add", args);
        fmt::print("   add(15, 25) = ");
        std::visit([](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, int>) {
                fmt::print("{}\n", arg);
            } else if constexpr (std::is_same_v<T, double>) {
                fmt::print("{}\n", arg);
            } else {
                fmt::print("unknown\n");
            }
        }, func_result->data);
        fmt::print("   ✓ PASS\n\n");

        // Test 4: Test hasFunction
        fmt::print("Test 4: Check function existence\n");
        bool has_add = py_exec.hasFunction("add");
        bool has_missing = py_exec.hasFunction("nonexistent");
        fmt::print("   hasFunction('add'): {}\n", has_add);
        fmt::print("   hasFunction('nonexistent'): {}\n", has_missing);
        if (has_add && !has_missing) {
            fmt::print("   ✓ PASS\n\n");
        } else {
            fmt::print("   ✗ FAIL\n\n");
        }

        fmt::print("=== All Tests Passed ===\n");
        return 0;

    } catch (const std::exception& e) {
        fmt::print("\n✗ TEST FAILED: {}\n", e.what());
        return 1;
    }
}
