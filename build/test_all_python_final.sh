#!/bin/bash
set -e

echo "🚀 Testing ALL Python async tests..."
echo ""

./naab_unit_tests --gtest_filter="PolyglotAsyncTest.Python*"

echo ""
echo "✅ All Python async tests completed!"
