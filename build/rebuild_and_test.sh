#!/bin/bash
set -e

echo "=== Rebuilding with compilation fix ==="
cmake --build . --target naab_unit_tests -j4 2>&1 | tail -20

echo ""
echo "=== Running Python async test ==="
timeout 10 ./naab_unit_tests --gtest_filter="PolyglotAsyncTest.PythonWithReturn" && {
    echo ""
    echo "🎉🎉🎉 IT WORKS! 🎉🎉🎉"
} || {
    echo "Still failing..."
}
