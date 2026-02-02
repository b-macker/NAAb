#!/bin/bash
set -e

echo "Testing Shell timeout failure..."
echo ""

./naab_unit_tests --gtest_filter="PolyglotAsyncTest.ShellWithTimeout" 2>&1
