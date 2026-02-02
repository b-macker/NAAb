#!/bin/bash
set -e

echo "=== Rebuilding with extra debug logging ==="
touch ../src/runtime/polyglot_async_executor.cpp
cmake --build . --target naab_unit_tests 2>&1 | tail -5

echo ""
echo "=== Running test with detailed logging ==="
timeout 10 ./naab_unit_tests --gtest_filter="PolyglotAsyncTest.PythonWithReturn"
