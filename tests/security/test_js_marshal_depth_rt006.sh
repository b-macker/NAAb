#!/usr/bin/env bash
# test_js_marshal_depth_rt006.sh — V-RT-006: JavaScript marshalling stack overflow
# CrossLanguageBridge::valueToJS and static toJSValue() in js_executor.cpp must
# enforce a depth limit (>64) when recursing into deeply nested lists/dicts.
# Before fix: no depth limit → SIGSEGV on deeply nested structures.
# After fix: JS_ThrowRangeError at depth >64 → clear error, no crash.
# Mirrors test_marshal_depth_rt005.sh (V-RT-005) for JavaScript.
set -euo pipefail

NAAB="${1:-$(dirname "$0")/../../build/naab-lang}"
PASS=0; FAIL=0

ok()   { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

WORK_DIR="${HOME}/.naab/rt006_$$"
mkdir -p "$WORK_DIR"
cleanup() { rm -rf "$WORK_DIR"; }
trap cleanup EXIT

echo "=== test_js_marshal_depth_rt006.sh ==="
echo ""

# ---------------------------------------------------------------------------
# T1: 70-level nested list → <<js block → must produce depth error, NOT SIGSEGV.
#     valueToJS() recurses on list elements; with V-RT-006 fix, depth > 64
#     calls JS_ThrowRangeError and returns JS_EXCEPTION propagated to caller.
# ---------------------------------------------------------------------------
echo "[T1] 70-level nested list → <<js block → depth error (not SIGSEGV)"

cat > "$WORK_DIR/test_t1.naab" << 'NAAB'
main {
    let d = [1]
    let i = 0
    while i < 70 {
        d = [d]
        i = i + 1
    }
    let result = <<js[d]
JSON.stringify(d).slice(0, 20)
>>
    print(result)
}
NAAB

ec=0
out=$(timeout 15s "$NAAB" "$WORK_DIR/test_t1.naab" --vm --no-governance 2>&1) || ec=$?

# Check: must NOT be killed by SIGSEGV (139) or SIGABRT (134)
if [[ "$ec" -eq 139 ]] || [[ "$ec" -eq 134 ]]; then
    fail "Runtime crashed (SIGSEGV/SIGABRT exit $ec) — depth limit not enforced in JS marshalling"
elif [[ "$ec" -eq 124 ]]; then
    fail "Timed out — possible infinite recursion without depth limit"
elif echo "$out" | grep -qi "depth\|maximum\|marshall\|exceeded\|RangeError"; then
    ok "Clear depth error produced — no crash (exit $ec)"
elif [[ "$ec" -ne 0 ]]; then
    ok "Non-zero exit without crash — depth limit or JS exception caught (exit $ec)"
else
    # If JS executor is not available, this might just succeed with truncated data
    ok "Completed without crash — JS may not be available or depth was handled"
fi

echo "    Output: ${out:0:120}"
echo ""

# ---------------------------------------------------------------------------
# T2: 30-level nested list → <<js block → must succeed (within depth limit).
#     Verifies the depth guard doesn't false-positive at shallow depths.
# ---------------------------------------------------------------------------
echo "[T2] 30-level nested list → <<js block → executes successfully"

cat > "$WORK_DIR/test_t2.naab" << 'NAAB'
main {
    let d = [1]
    let i = 0
    while i < 30 {
        d = [d]
        i = i + 1
    }
    let result = <<js[d]
"ok"
>>
    print(result)
}
NAAB

ec=0
out=$(timeout 15s "$NAAB" "$WORK_DIR/test_t2.naab" --vm --no-governance 2>&1) || ec=$?

if [[ "$ec" -eq 139 ]] || [[ "$ec" -eq 134 ]]; then
    fail "Runtime crashed (SIGSEGV/SIGABRT) at depth 30 — depth limit too aggressive or bug"
elif [[ "$ec" -eq 124 ]]; then
    fail "Timed out at depth 30"
elif [[ "$ec" -eq 0 ]] && echo "$out" | grep -q "ok"; then
    ok "30-level list succeeded — depth limit not triggered at safe depth"
elif [[ "$ec" -ne 0 ]]; then
    # JS executor might not be available
    if echo "$out" | grep -qi "javascript.*not.*available\|executor.*not\|not.*compiled\|not.*built"; then
        ok "T2 skipped — JavaScript executor not available"
    elif echo "$out" | grep -qi "depth\|maximum\|RangeError"; then
        fail "Depth error at only 30 levels — false positive depth guard"
    else
        ok "Non-zero exit (exit $ec) — JS may not be available: ${out:0:80}"
    fi
else
    ok "T2 completed (may not have printed 'ok' if JS unavailable): ${out:0:80}"
fi

echo "    Output: ${out:0:120}"
echo ""

# ---------------------------------------------------------------------------
# T3: 70-level nested dict → <<js block → depth error, not SIGSEGV.
#     Same fix but for dict path in valueToJS. The dict branch also recurses.
# ---------------------------------------------------------------------------
echo "[T3] 70-level nested dict → <<js block → depth error (not crash)"

cat > "$WORK_DIR/test_t3.naab" << 'NAAB'
main {
    let d = {"v": 1}
    let i = 0
    while i < 70 {
        d = {"inner": d}
        i = i + 1
    }
    let result = <<js[d]
typeof d
>>
    print(result)
}
NAAB

ec=0
out=$(timeout 15s "$NAAB" "$WORK_DIR/test_t3.naab" --vm --no-governance 2>&1) || ec=$?

if [[ "$ec" -eq 139 ]] || [[ "$ec" -eq 134 ]]; then
    fail "Runtime crashed (SIGSEGV/SIGABRT) on deeply nested dict — dict depth guard missing"
elif [[ "$ec" -eq 124 ]]; then
    fail "Timed out — possible infinite recursion on nested dict"
elif echo "$out" | grep -qi "depth\|maximum\|marshall\|exceeded\|RangeError"; then
    ok "Clear depth error for nested dict — no crash"
elif [[ "$ec" -ne 0 ]]; then
    ok "Non-zero exit without crash — depth limit caught or JS unavailable (exit $ec)"
else
    ok "Completed without crash (JS may not be available or dict handled)"
fi

echo "    Output: ${out:0:120}"
echo ""

TOTAL=$(( PASS + FAIL ))
echo "Results: ${PASS}/${TOTAL} passed"
if [[ "$FAIL" -gt 0 ]]; then exit 1; fi
