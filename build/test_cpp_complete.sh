#!/bin/bash
set -e

echo "=== Testing C++ with full output ==="
echo ""

echo "Test 1: CppSimpleExecution"
timeout 30 ./naab_unit_tests --gtest_filter="PolyglotAsyncTest.CppSimpleExecution" 2>&1 | tail -15

echo ""
echo "Test 2: CppBlockingExecution"
timeout 30 ./naab_unit_tests --gtest_filter="PolyglotAsyncTest.CppBlockingExecution" 2>&1 | tail -15
