#!/usr/bin/env bash
# Security R32 Fix Verification Tests
# V-CONC-007: Governance results_mutex_
# V-GOV-024:  Telemetry cap MAX_CHECK_RESULTS
# V-CONC-006: NaabVal deepCopy for async isolation
# V-DOS-013:  JSON struct cycle detection
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
WORK_DIR="$(mktemp -d "${TMPDIR:-/data/data/com.termux/files/usr/tmp}/naab_r32.XXXXXX")"
cleanup() { rm -rf "$WORK_DIR"; }
trap cleanup EXIT

PASS=0
FAIL=0

pass() { echo "PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "FAIL: $1"; [[ -n "${2:-}" ]] && echo "  $2"; FAIL=$((FAIL+1)); }

cat > "$WORK_DIR/govern.json" << 'JSON'
{ "version": "4.0", "mode": "off" }
JSON

# ── V-CONC-007: Governance results_mutex_ ──────────────────────────────────

echo "=== V-CONC-007: Governance thread safety ==="

SRC_GOV_H="$SCRIPT_DIR/../../include/naab/governance.h"
SRC_GOV_CPP="$SCRIPT_DIR/../../src/runtime/governance_engine.cpp"

# Test 1: results_mutex_ declared in governance.h
if grep -q 'results_mutex_' "$SRC_GOV_H"; then
    pass "V-CONC-007 T1: results_mutex_ declared in governance.h"
else
    fail "V-CONC-007 T1: results_mutex_ not found in governance.h"
fi

# Test 2: lock_guard used around check_results_ writes
if grep -B2 'check_results_.push_back' "$SRC_GOV_CPP" | grep -q 'lock_guard.*results_mutex_'; then
    pass "V-CONC-007 T2: check_results_ writes are mutex-guarded"
else
    fail "V-CONC-007 T2: check_results_ writes NOT mutex-guarded"
fi

# ── V-GOV-024: Telemetry cap ──────────────────────────────────────────────

echo "=== V-GOV-024: Telemetry memory cap ==="

# Test 3: MAX_CHECK_RESULTS defined
if grep -q 'MAX_CHECK_RESULTS' "$SRC_GOV_H"; then
    pass "V-GOV-024 T3: MAX_CHECK_RESULTS defined"
else
    fail "V-GOV-024 T3: MAX_CHECK_RESULTS not found"
fi

# Test 4: Eviction logic after push_back
if grep -A3 'check_results_.push_back' "$SRC_GOV_CPP" | grep -q 'MAX_CHECK_RESULTS\|erase'; then
    pass "V-GOV-024 T4: Eviction logic present after push_back"
else
    fail "V-GOV-024 T4: No eviction logic found"
fi

# ── V-CONC-006: NaabVal deepCopy ──────────────────────────────────────────

echo "=== V-CONC-006: Async container isolation ==="

SRC_NAAB_VAL_H="$SCRIPT_DIR/../../include/naab/naab_val.h"
SRC_NAAB_VAL_CPP="$SCRIPT_DIR/../../src/interpreter/naab_val.cpp"
SRC_VM="$SCRIPT_DIR/../../src/vm/vm.cpp"

# Test 5: deepCopy declared in naab_val.h
if grep -q 'deepCopy' "$SRC_NAAB_VAL_H"; then
    pass "V-CONC-006 T5: deepCopy declared in naab_val.h"
else
    fail "V-CONC-006 T5: deepCopy not found in naab_val.h"
fi

# Test 6: deepCopy implemented in naab_val.cpp
if grep -q 'NaabVal::deepCopy' "$SRC_NAAB_VAL_CPP"; then
    pass "V-CONC-006 T6: deepCopy implemented in naab_val.cpp"
else
    fail "V-CONC-006 T6: deepCopy not found in naab_val.cpp"
fi

# Test 7: VM async uses deepCopy for globals
if grep -q 'deepCopy' "$SRC_VM"; then
    pass "V-CONC-006 T7: VM async uses deepCopy"
else
    fail "V-CONC-006 T7: VM async does NOT use deepCopy"
fi

# ── V-DOS-013: JSON struct cycle detection ─────────────────────────────────

echo "=== V-DOS-013: JSON struct cycle detection ==="

SRC_JSON="$SCRIPT_DIR/../../src/stdlib/json_impl.cpp"

# Test 8: isStructVal branch has visited check
if grep -A15 'isStructVal' "$SRC_JSON" | grep -q 'visited.*insert\|circular reference'; then
    pass "V-DOS-013 T8: Struct branch has visited/cycle check"
else
    fail "V-DOS-013 T8: Struct branch missing cycle check"
fi

# Test 9: Runtime — json.stringify on deeply nested struct doesn't crash
cat > "$WORK_DIR/struct_depth.naab" << 'NAAB'
struct Node {
    value: int,
    next: any
}
main {
    let a = new Node { value: 1, next: null }
    let b = new Node { value: 2, next: a }
    let c = new Node { value: 3, next: b }
    let result = json.stringify(c)
    print(result)
}
NAAB

OUTPUT=$("$NAAB" "$WORK_DIR/struct_depth.naab" 2>&1 || true)
if echo "$OUTPUT" | grep -q '"value":3'; then
    pass "V-DOS-013 T9: Nested struct serialization works"
else
    fail "V-DOS-013 T9: Nested struct serialization failed" "$OUTPUT"
fi

# ── Summary ────────────────────────────────────────────────────────────────

echo ""
echo "================================"
echo "R32 Results: $PASS passed, $FAIL failed (of $((PASS+FAIL)) tests)"
echo "================================"

[[ $FAIL -eq 0 ]] && exit 0 || exit 1
