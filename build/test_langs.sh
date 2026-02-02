#!/bin/bash

tests=(
    "CppSimpleExecution"
    "CSharpSimpleExecution"
    "RustBlockingExecution"
    "ShellSimpleExecution"
    "GenericSubprocessRuby"
)

for test in "${tests[@]}"; do
    echo "=== Testing $test ==="
    ./naab_unit_tests --gtest_filter="PolyglotAsyncTest.$test" 2>&1 | grep -E "RUN|OK|FAILED"
    echo ""
done
