#!/bin/bash
set -e

echo "Testing Python exception handling..."
echo ""

./naab_unit_tests --gtest_filter="PolyglotAsyncTest.PythonException" 2>&1

echo ""
echo "Test completed."
