#!/bin/bash
set -e

echo "=== Rebuilding with exception fix ==="
touch ../src/runtime/python_executor.cpp
cmake --build . --target naab_unit_tests 2>&1 | tail -5

echo ""
echo "=== Testing Python exception handling ==="
./naab_unit_tests --gtest_filter="PolyglotAsyncTest.PythonException"
