#!/bin/bash
# Distributed Systems Testing: Governance Invariant Checker
# Verifies NAAb governance behaves correctly under "distributed" conditions
# (async interpreters, persistent runtimes, module loading, GC stress)

set -e

NAAB_BIN="./build/naab-lang"
TEST_DIR="tests/governance_v4"
PASSED=0
FAILED=0
TOTAL=0

echo "═══════════════════════════════════════════════════════════"
echo "  NAAb Governance - Distributed Systems Invariant Checks"
echo "═══════════════════════════════════════════════════════════"
echo ""

# Helper function to run a test and check for PASS
run_test() {
    local test_file="$1"
    local test_name=$(basename "$test_file" .naab)

    TOTAL=$((TOTAL + 1))
    echo -n "[$TOTAL] Testing $test_name... "

    if timeout 30 "$NAAB_BIN" "$test_file" 2>&1 | grep -q "^PASS"; then
        PASSED=$((PASSED + 1))
        echo "✓ PASS"
    else
        FAILED=$((FAILED + 1))
        echo "✗ FAIL"
    fi
}

echo "Phase 1: State Consistency Tests"
echo "-----------------------------------------------------------"

# Scenario 3: Stale lastReturnTainted across calls
run_test "$TEST_DIR/test_return_taint_reset.naab"

# Scenario 1: Async taint state divergence (from Phase 2)
run_test "$TEST_DIR/test_async_governance.naab"

# Scenario 5: Nested async taint propagation (documented limitation)
run_test "$TEST_DIR/test_nested_async_taint.naab"

# Scenario 10: Catch variable taint collision (from Phase 2)
run_test "$TEST_DIR/test_exception_taint.naab"

# Scenario 11: GC during nested cross-module calls
run_test "$TEST_DIR/test_gc_nested_modules.naab"

echo ""
echo "Phase 2: Bypass Path Tests"
echo "-----------------------------------------------------------"

# Scenario 2: Persistent runtime governance bypass (from Phase 2)
run_test "$TEST_DIR/test_persistent_runtime_gov.naab"

# Expression-level taint at sinks (from Phase 2)
run_test "$TEST_DIR/test_expression_sink_bypass.naab"

echo ""
echo "Phase 3: Additional Governance Tests"
echo "-----------------------------------------------------------"

# String interpolation taint (from Phase 2)
run_test "$TEST_DIR/test_interpolation_taint.naab"

# Container taint (from Phase 2)
run_test "$TEST_DIR/test_container_taint.naab"

# Pipeline taint (from Phase 2)
run_test "$TEST_DIR/test_pipeline_taint.naab"

# Nested return taint (from Phase 2)
run_test "$TEST_DIR/test_nested_return_taint.naab"

echo ""
echo "═══════════════════════════════════════════════════════════"
echo "  INVARIANT CHECK SUMMARY"
echo "═══════════════════════════════════════════════════════════"
echo ""
echo "Total Tests:  $TOTAL"
echo "Passed:       $PASSED"
echo "Failed:       $FAILED"
echo ""

if [ $FAILED -eq 0 ]; then
    echo "✓ ALL INVARIANTS VERIFIED"
    echo ""
    echo "Verified Invariants:"
    echo "  1. Taint consistency - expressions containing tainted vars are tainted"
    echo "  2. Return taint reset - clean functions don't inherit stale taint"
    echo "  3. Async isolation - each async interpreter has independent state"
    echo "  4. No silent bypass - all polyglot blocks enforce governance"
    echo "  5. GC safety - no dict/list corruption during nested module calls"
    echo ""
    exit 0
else
    echo "✗ INVARIANT VIOLATIONS DETECTED"
    echo ""
    echo "Failed tests: $FAILED/$TOTAL"
    echo ""
    exit 1
fi
