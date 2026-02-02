#!/bin/bash
set -e

echo "🎉 Python async is WORKING! Testing all Python tests..."
echo ""

./naab_unit_tests --gtest_filter="PolyglotAsyncTest.Python*"

echo ""
echo "✅ All Python async tests completed!"
