#!/bin/bash
set -e

echo "=== Rebuilding with explicit cleanup fix ==="
cmake --build . -j4 2>&1 | tail -8

echo ""
echo "=== Testing Python async ==="
./naab_unit_tests --gtest_filter="PolyglotAsyncTest.PythonWithReturn"

echo ""
echo "SUCCESS! Running all Python tests..."
./naab_unit_tests --gtest_filter="PolyglotAsyncTest.Python*"
