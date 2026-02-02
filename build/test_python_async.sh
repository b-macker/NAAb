#!/bin/bash
# Test Python async execution after ScopedTimeout fix

echo "Testing Python async execution..."
echo ""

./naab_unit_tests --gtest_filter="PolyglotAsyncTest.Python*"

exit $?
