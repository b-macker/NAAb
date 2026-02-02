#!/bin/bash
set -e

echo "=== Cleaning and rebuilding ==="
make clean
touch ../src/runtime/python_executor.cpp
cmake --build . --target naab_unit_tests -j4

echo ""
echo "=== Testing ==="
timeout 10 ./naab_unit_tests --gtest_filter="PolyglotAsyncTest.PythonWithReturn"
