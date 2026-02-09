#!/bin/bash

cd /data/data/com.termux/files/home/.naab/language

passed=0
failed=0
declare -a failed_tests

echo "Running 66 verification tests..."
echo "================================"

# Ch01
tests=(
"docs/book/verification/ch01_intro/hello.naab"
"docs/book/verification/ch02_basics/advanced_types.naab"
"docs/book/verification/ch02_basics/dictionaries.naab"
"docs/book/verification/ch02_basics/structs_vs_dicts.naab"
"docs/book/verification/ch02_basics/types_and_structs.naab"
"docs/book/verification/ch03_control/control_flow.naab"
"docs/book/verification/ch04_functions/functions.naab"
"docs/book/verification/ch04_functions/math_utils.naab"
"docs/book/verification/ch04_functions/module_imports.naab"
"docs/book/verification/ch04_functions/polyglot_in_exports.naab"
"docs/book/verification/ch04_functions/stats_module.naab"
"docs/book/verification/ch04_functions/stdlib_usage.naab"
"docs/book/verification/ch04_functions/test_modules.naab"
"docs/book/verification/ch05_polyglot/import_polyglot_module.naab"
"docs/book/verification/ch05_polyglot/polyglot_basics.naab"
"docs/book/verification/ch05_polyglot/polyglot_in_functions.naab"
"docs/book/verification/ch05_polyglot/polyglot_module.naab"
"docs/book/verification/ch05_polyglot/test_module_helper.naab"
)

# Add all tests in ch0_full_projects
for test in docs/book/verification/ch0_full_projects/**/*.naab; do
    tests+=("$test")
done

total=${#tests[@]}

for test in "${tests[@]}"; do
    if [ -f "$test" ]; then
        timeout 5s build/naab-lang run "$test" > /dev/null 2>&1
        if [ $? -eq 0 ]; then
            echo "✓ $test"
            ((passed++))
        else
            echo "✗ $test"
            failed_tests+=("$test")
            ((failed++))
        fi
    fi
done

echo ""
echo "================================"
echo "Results: $passed passed, $failed failed out of $total tests"
echo ""

if [ $failed -gt 0 ]; then
    echo "Failed tests:"
    for t in "${failed_tests[@]}"; do
        echo "  - $t"
    done
    exit 1
else
    echo "✅ ALL TESTS PASSED!"
    exit 0
fi
