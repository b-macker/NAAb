#!/bin/bash
set -e

echo "=== Force rebuilding python_executor ==="
touch ../src/runtime/python_executor.cpp
cmake --build . --target naab_unit_tests -j4 2>&1 | tail -15

echo ""
echo "=== Testing with forced rebuild ==="
timeout 10 ./naab_unit_tests --gtest_filter="PolyglotAsyncTest.PythonWithReturn"
