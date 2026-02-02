#!/bin/bash
set -e

echo "=== Rebuilding with destructor fix ==="
cmake --build . -j4 2>&1 | grep -E "(Built target|Linking)" | tail -5

echo ""
echo "=== Running Python async test ==="
./naab_unit_tests --gtest_filter="PolyglotAsyncTest.PythonWithReturn"

echo ""
echo "=== Running all Python tests ==="
./naab_unit_tests --gtest_filter="PolyglotAsyncTest.Python*"
