#!/bin/bash
# Property-Based Test Runner
# Runs all five invariant suites:
#   1. Governance transparency (A/B test)
#   2. Taint monotonicity
#   3. Type error completeness
#   4. Polyglot differential (NAAb executor vs raw Python)
#   5. Serialization boundary audit

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB_BIN="${NAAB_BIN:-$PROJECT_DIR/build/naab-lang}"

export NAAB_BIN

if [ ! -f "$NAAB_BIN" ]; then
    echo "Error: naab-lang binary not found at $NAAB_BIN"
    echo "Run 'cd build && make naab-lang' first"
    exit 1
fi

echo "==============================================="
echo "  NAAb Property-Based Tests"
echo "==============================================="
echo ""

SUITES_PASSED=0
SUITES_FAILED=0

# --- Invariant 1: Governance Transparency ---
echo ""
if bash "$SCRIPT_DIR/test_governance_transparency.sh"; then
    SUITES_PASSED=$((SUITES_PASSED + 1))
else
    SUITES_FAILED=$((SUITES_FAILED + 1))
fi

# --- Invariant 2: Taint Monotonicity ---
echo ""
echo "--- Invariant 2: Taint Monotonicity ---"
if timeout 30s "$NAAB_BIN" run "$SCRIPT_DIR/test_taint_monotonicity.naab" 2>&1; then
    SUITES_PASSED=$((SUITES_PASSED + 1))
else
    echo "PROPERTY VIOLATED or test crashed!"
    SUITES_FAILED=$((SUITES_FAILED + 1))
fi

# --- Invariant 3: Type Error Completeness ---
echo ""
if bash "$SCRIPT_DIR/test_type_completeness.sh"; then
    SUITES_PASSED=$((SUITES_PASSED + 1))
else
    SUITES_FAILED=$((SUITES_FAILED + 1))
fi

# --- Invariant 4: Polyglot Differential ---
echo ""
if bash "$SCRIPT_DIR/test_polyglot_differential.sh"; then
    SUITES_PASSED=$((SUITES_PASSED + 1))
else
    SUITES_FAILED=$((SUITES_FAILED + 1))
fi

# --- Invariant 5: Serialization Boundary Audit ---
echo ""
if bash "$SCRIPT_DIR/test_serialization_audit.sh"; then
    SUITES_PASSED=$((SUITES_PASSED + 1))
else
    SUITES_FAILED=$((SUITES_FAILED + 1))
fi

# --- Invariant 6: Governance Properties ---
echo ""
if bash "$SCRIPT_DIR/test_governance_properties.sh"; then
    SUITES_PASSED=$((SUITES_PASSED + 1))
else
    SUITES_FAILED=$((SUITES_FAILED + 1))
fi

# Summary
echo ""
echo "==============================================="
echo "  PROPERTY TEST SUMMARY"
echo "==============================================="
echo "  Suites passed: $SUITES_PASSED / $((SUITES_PASSED + SUITES_FAILED))"
if [ $SUITES_FAILED -eq 0 ]; then
    echo "  ALL INVARIANTS HOLD"
    exit 0
else
    echo "  $SUITES_FAILED INVARIANT(S) VIOLATED"
    exit 1
fi
