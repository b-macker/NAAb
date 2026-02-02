#!/bin/bash
set -e

echo "🚀 Running ALL polyglot async tests (final check)..."
echo ""

timeout 120 ./naab_unit_tests --gtest_filter="PolyglotAsyncTest.*" 2>&1 | tail -50

echo ""
echo "✅ Test run completed!"
