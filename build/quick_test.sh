#!/bin/bash
set -e

echo "=== Quick rebuild and test ==="
cmake --build . -j4 > /dev/null 2>&1 || {
    echo "Build failed!"
    cmake --build . 2>&1 | tail -50
    exit 1
}

echo "Build successful!"
echo ""
echo "=== Running Python async tests ==="
./naab_unit_tests --gtest_filter="PolyglotAsyncTest.Python*"
