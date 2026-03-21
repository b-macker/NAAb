#!/bin/bash
# Robustness test runner — runs all categories, captures output for JSON report

NAAB_BIN="./build/naab-lang"
DIR="tests/robustness"
RESULTS_FILE="$HOME/.naab_robustness_results.txt"

> "$RESULTS_FILE"

echo "═══════════════════════════════════════════════════════════"
echo "  NAAb Robustness Test Suite"
echo "═══════════════════════════════════════════════════════════"
echo ""

TOTAL_PASS=0
TOTAL_FAIL=0
TOTAL_TESTS=0

for test_file in \
    "$DIR/test_instruction_following.naab" \
    "$DIR/test_factuality.naab" \
    "$DIR/test_injection_resilience.naab" \
    "$DIR/test_data_leakage.naab" \
    "$DIR/test_tool_safety.naab" \
    "$DIR/test_encoding_edge_cases.naab" \
    "$DIR/test_concurrency_state.naab" \
    "$DIR/test_crash_regression.naab" \
    "$DIR/test_security_bypass.naab" \
    "$DIR/test_stdlib_array.naab" \
    "$DIR/test_stdlib_string.naab" \
    "$DIR/test_stdlib_math_json.naab" \
    "$DIR/test_operators_matrix.naab" \
    "$DIR/test_closures_scope.naab" \
    "$DIR/test_control_flow.naab" \
    "$DIR/test_structs_enums.naab" \
    "$DIR/test_stdlib_env_time.naab" \
    "$DIR/test_mutations.naab" \
    "$DIR/test_sensitivity.naab"; do

    test_name=$(basename "$test_file" .naab)
    echo "--- $test_name ---"

    # Run with governance (tests in robustness/ dir use their own govern.json)
    output=$(timeout 120s "$NAAB_BIN" run "$test_file" 2>&1)
    exit_code=$?

    echo "$output" | grep -E "^\s+(T[0-9]+|test_|DETAIL)"

    # Extract summary line
    summary=$(echo "$output" | grep -E "^[A-Z].*: [0-9]+/[0-9]+" | tail -1)
    if [ -n "$summary" ]; then
        echo "  $summary"
        pass=$(echo "$summary" | grep -oP '(\d+)/' | tr -d '/')
        total=$(echo "$summary" | grep -oP '/(\d+)' | tr -d '/')
        fail=$((total - pass))
        TOTAL_PASS=$((TOTAL_PASS + pass))
        TOTAL_FAIL=$((TOTAL_FAIL + fail))
        TOTAL_TESTS=$((TOTAL_TESTS + total))
    elif [ $exit_code -ne 0 ]; then
        echo "  CRASH (exit code $exit_code)"
        echo "  First line: $(echo "$output" | head -1)"
        TOTAL_FAIL=$((TOTAL_FAIL + 1))
        TOTAL_TESTS=$((TOTAL_TESTS + 1))
    fi

    # Save full output
    echo "=== $test_name ===" >> "$RESULTS_FILE"
    echo "$output" >> "$RESULTS_FILE"
    echo "" >> "$RESULTS_FILE"

    echo ""
done

echo "═══════════════════════════════════════════════════════════"
echo "  ROBUSTNESS SUMMARY"
echo "═══════════════════════════════════════════════════════════"
echo "  Total:  $TOTAL_TESTS"
echo "  Pass:   $TOTAL_PASS"
echo "  Fail:   $TOTAL_FAIL"
echo ""
if [ $TOTAL_FAIL -eq 0 ]; then
    echo "  ALL TESTS PASSED"
else
    echo "  $TOTAL_FAIL FAILURES — see details above"
fi
echo ""
echo "Full output saved to: $RESULTS_FILE"
