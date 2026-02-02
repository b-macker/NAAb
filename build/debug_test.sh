#!/bin/bash
set -e

echo "=== Rebuilding with debug logging ==="
cmake --build . -j4 2>&1 | tail -10

echo ""
echo "=== Running single Python test with debug output ==="
./naab_unit_tests --gtest_filter="PolyglotAsyncTest.PythonWithReturn" 2>&1 | head -100
