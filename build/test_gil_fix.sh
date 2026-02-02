#!/bin/bash
set -e

echo "=== Rebuilding with GIL release fix ==="
cmake --build . -j4 2>&1 | tail -15

echo ""
echo "=== Testing Python async (should now work!) ==="
timeout 35 ./naab_unit_tests --gtest_filter="PolyglotAsyncTest.PythonWithReturn" || {
    echo "Test timed out or failed"
    exit 1
}

echo ""
echo "=== Success! Running all Python tests ==="
./naab_unit_tests --gtest_filter="PolyglotAsyncTest.Python*"
