#!/bin/bash
# Run type checker tests
# Error tests should fail with --strict-types
# Valid tests should pass with --strict-types

NAAB="./build/naab-lang"
PASS=0
FAIL=0
TOTAL=0

echo "═══════════════════════════════════════════════════════════"
echo "  TYPE CHECKER TESTS"
echo "═══════════════════════════════════════════════════════════"
echo ""

# Tests that SHOULD produce type errors (--strict-types should exit non-zero)
echo "--- Error detection tests (should catch type errors) ---"
for f in tests/type_system/errors/*.naab; do
    [ -f "$f" ] || continue
    TOTAL=$((TOTAL + 1))
    output=$($NAAB --strict-types "$f" 2>&1)
    if [ $? -ne 0 ]; then
        echo "  PASS: $(basename $f) — type error caught"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $(basename $f) — should have type errors but passed"
        FAIL=$((FAIL + 1))
    fi
done

echo ""

# Tests that should PASS (correctly typed or untyped gradual code)
echo "--- Valid code tests (should pass) ---"
for f in tests/type_system/valid/*.naab; do
    [ -f "$f" ] || continue
    TOTAL=$((TOTAL + 1))
    output=$($NAAB "$f" 2>&1)
    if [ $? -eq 0 ]; then
        echo "  PASS: $(basename $f) — runs without errors"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $(basename $f) — unexpected error: $output"
        FAIL=$((FAIL + 1))
    fi
done

echo ""

# Also test that valid code passes --strict-types
echo "--- Strict mode on valid code (should still pass) ---"
for f in tests/type_system/valid/*.naab; do
    [ -f "$f" ] || continue
    TOTAL=$((TOTAL + 1))
    output=$($NAAB --strict-types "$f" 2>&1)
    if [ $? -eq 0 ]; then
        echo "  PASS: $(basename $f) --strict-types — no type errors"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $(basename $f) --strict-types — false positive: $output"
        FAIL=$((FAIL + 1))
    fi
done

echo ""
echo "═══════════════════════════════════════════════════════════"
echo "  RESULTS: $PASS/$TOTAL passed, $FAIL failed"
echo "═══════════════════════════════════════════════════════════"

if [ $FAIL -gt 0 ]; then
    exit 1
fi
exit 0
