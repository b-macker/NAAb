#!/bin/bash
set -e

echo "🚀 Running ALL polyglot async tests..."
echo ""

./naab_unit_tests --gtest_filter="PolyglotAsyncTest.*"

echo ""
echo "✅ All polyglot async tests completed!"
