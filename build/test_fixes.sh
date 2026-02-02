#!/bin/bash
set -e

echo "=== Rebuilding with Shell and C++ fixes ==="
touch ../src/runtime/shell_executor.cpp ../src/runtime/cpp_executor.cpp
cmake --build . --target naab_unit_tests 2>&1 | tail -8

echo ""
echo "=== Testing Shell timeout (should pass now) ==="
./naab_unit_tests --gtest_filter="PolyglotAsyncTest.ShellWithTimeout" 2>&1 | grep -E "(RUN|OK|FAILED|PASSED)"

echo ""
echo "=== Testing C++ execution (should pass now) ==="
./naab_unit_tests --gtest_filter="PolyglotAsyncTest.CppSimpleExecution" 2>&1 | grep -E "(RUN|OK|FAILED|PASSED|error:)"

echo ""
echo "=== Testing C++ blocking (should pass now) ==="
./naab_unit_tests --gtest_filter="PolyglotAsyncTest.CppBlockingExecution" 2>&1 | grep -E "(RUN|OK|FAILED|PASSED|error:)"
