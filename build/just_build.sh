#!/bin/bash
set -e

echo "=== Building (no clean) ==="
touch ../src/runtime/python_executor.cpp
cmake --build . --target naab_unit_tests -j4 2>&1 | tail -30

echo ""
echo "Build complete, testing..."
timeout 10 ./naab_unit_tests --gtest_filter="PolyglotAsyncTest.PythonWithReturn"
