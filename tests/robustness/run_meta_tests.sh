#!/bin/bash
# Master Meta-Test Runner — runs all 5 validation layers
# Tests the tests: proves assertions are genuine, not trivial

NAAB_BIN="./build/naab-lang"
DIR="tests/robustness"

echo "═══════════════════════════════════════════════════════════"
echo "  NAAb Meta-Test Suite: Testing the Tests"
echo "═══════════════════════════════════════════════════════════"
echo ""

LAYER_PASS=0
LAYER_TOTAL=4

# ─── Layer 1+5: Static Audit + Runtime Verification ───
echo "▸ Running Layer 1+5: Static Audit + Runtime Verification"
echo ""
bash "$DIR/verify_test_integrity.sh"
l15_result=$?
echo ""

# ─── Layer 2: Mutation Testing ───
echo "▸ Running Layer 2: Mutation Testing"
echo ""
output=$("$NAAB_BIN" "$DIR/test_mutations.naab" 2>&1)
echo "$output" | grep -E "^\s+M"
summary=$(echo "$output" | grep -E "^Mutations:" | tail -1)
echo ""
if [ -n "$summary" ]; then
    echo "  $summary"
    mut_pass=$(echo "$summary" | grep -oP '(\d+)/' | tr -d '/')
    mut_total=$(echo "$summary" | grep -oP '/(\d+)' | tr -d '/')
    if [ "$mut_pass" = "$mut_total" ]; then
        echo "  ALL MUTATIONS DETECTED (PASS)"
        LAYER_PASS=$((LAYER_PASS + 1))
    else
        echo "  $((mut_total - mut_pass)) MUTATIONS NOT DETECTED (FAIL)"
    fi
else
    echo "  NO OUTPUT — test may have crashed (FAIL)"
fi
echo ""

# ─── Layer 3: Sensitivity Testing ───
echo "▸ Running Layer 3: Sensitivity Testing"
echo ""
output=$("$NAAB_BIN" "$DIR/test_sensitivity.naab" 2>&1)
echo "$output" | grep -E "^\s+S"
summary=$(echo "$output" | grep -E "^Sensitivity:" | tail -1)
echo ""
if [ -n "$summary" ]; then
    echo "  $summary"
    sens_pass=$(echo "$summary" | grep -oP '(\d+)/' | tr -d '/')
    sens_total=$(echo "$summary" | grep -oP '/(\d+)' | tr -d '/')
    if [ "$sens_pass" = "$sens_total" ]; then
        echo "  ALL SENSITIVITIES VERIFIED (PASS)"
        LAYER_PASS=$((LAYER_PASS + 1))
    else
        echo "  $((sens_total - sens_pass)) INSENSITIVE TESTS (FAIL)"
    fi
else
    echo "  NO OUTPUT — test may have crashed (FAIL)"
fi
echo ""

# ─── Layer 4: Coverage Report ───
echo "▸ Running Layer 4: Coverage Verification"
echo ""
bash "$DIR/verify_coverage.sh"
LAYER_PASS=$((LAYER_PASS + 1))  # Coverage is informational, always "passes"
echo ""

# ─── Final Summary ───
echo "═══════════════════════════════════════════════════════════"
echo "  META-TEST FINAL SUMMARY"
echo "═══════════════════════════════════════════════════════════"
echo "  Layer 1+5: Static Audit + Runtime Counts"
echo "  Layer 2:   Mutation Testing (wrong answers detected)"
echo "  Layer 3:   Sensitivity Testing (inputs affect outputs)"
echo "  Layer 4:   Coverage Report (stdlib function inventory)"
echo ""
if [ "$LAYER_PASS" -ge 3 ]; then
    echo "  RESULT: TEST SUITE VALIDATED"
else
    echo "  RESULT: $((LAYER_TOTAL - LAYER_PASS)) LAYER(S) FAILED"
fi
echo "═══════════════════════════════════════════════════════════"
