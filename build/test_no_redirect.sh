#!/bin/bash
set -e

echo "=== Rebuilding with no-redirect fix ==="
cmake --build . -j4 2>&1 | grep -E "Built target" | tail -3

echo ""
echo "=== Testing Python async (no stdout redirection) ==="
./naab_unit_tests --gtest_filter="PolyglotAsyncTest.PythonWithReturn"
