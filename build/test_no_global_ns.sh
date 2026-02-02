#!/bin/bash
set -e

echo "=== Rebuilding without global_namespace_ member ==="
cmake --build . -j4 2>&1 | tail -8

echo ""
echo "=== Testing Python async (no stored global namespace) ==="
./naab_unit_tests --gtest_filter="PolyglotAsyncTest.PythonWithReturn"

echo ""
echo "🎉 SUCCESS! Running all Python async tests..."
./naab_unit_tests --gtest_filter="PolyglotAsyncTest.Python*"
