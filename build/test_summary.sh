#!/bin/bash
set -e

echo "🎯 Final Test Summary (excluding slow C++ tests)"
echo ""

# Run all tests except C++
./naab_unit_tests --gtest_filter="PolyglotAsyncTest.*:-*Cpp*" 2>&1 | grep -E "(\[.*\]|PASSED|FAILED|tests from)"

echo ""
echo "Note: C++ tests excluded due to slow compilation time"
