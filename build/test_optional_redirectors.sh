#!/bin/bash
set -e

echo "=== Rebuilding with optional redirectors ==="
cmake --build . -j4 2>&1 | tail -5

echo ""
echo "=== Testing Python async (no redirector objects in async mode) ==="
timeout 10 ./naab_unit_tests --gtest_filter="PolyglotAsyncTest.PythonWithReturn" && {
    echo ""
    echo "🎉🎉🎉 SUCCESS! Test passed! 🎉🎉🎉"
    echo ""
    echo "=== Running ALL Python async tests ==="
    ./naab_unit_tests --gtest_filter="PolyglotAsyncTest.Python*"
} || {
    echo "Test failed or timed out"
    exit 1
}
