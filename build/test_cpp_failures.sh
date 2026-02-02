#!/bin/bash
set -e

echo "Testing C++ async failures..."
echo ""

echo "=== Test 1: CppSimpleExecution ==="
./naab_unit_tests --gtest_filter="PolyglotAsyncTest.CppSimpleExecution" 2>&1 | tail -20

echo ""
echo "=== Test 2: CppBlockingExecution ==="
./naab_unit_tests --gtest_filter="PolyglotAsyncTest.CppBlockingExecution" 2>&1 | tail -20
