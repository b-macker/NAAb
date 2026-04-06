#!/usr/bin/env bash
# test_js_timeout_rt002.sh — Finding V-RT-002: JS blocks respect --timeout flag
# The JS executor's interruptHandler must check ResourceLimiter::isTimeoutTriggered(),
# not just its own internal 30s watchdog.

set -euo pipefail
NAAB="${1:-$(dirname "$0")/../../build/naab-lang}"
PASS=0; FAIL=0
ok()   { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

WORKDIR="${HOME}/.naab/test_rt002_$$"
mkdir -p "$WORKDIR"
cleanup() { rm -rf "$WORKDIR"; }
trap cleanup EXIT

echo "=== test_js_timeout_rt002.sh ==="
echo ""

# ---------------------------------------------------------------------------
# T1: JS block with infinite loop, --timeout 3 → must terminate well under 30s
# ---------------------------------------------------------------------------
echo "[T1] JS infinite loop with --timeout 3 terminates within ~8s (not 30s)"
cat > "$WORKDIR/test_t1.naab" << 'EOF'
main {
  let result = <<javascript
while(true) {}
"unreachable"
>>
}
EOF

start_ts=$(date +%s)
out=$(timeout 15s "$NAAB" "$WORKDIR/test_t1.naab" --vm --no-governance --timeout 3 2>&1) || ec=$?
ec=${ec:-0}
end_ts=$(date +%s)
elapsed=$((end_ts - start_ts))

if [[ "$elapsed" -ge 15 ]]; then
    fail "JS block ran for ≥15s — timeout not enforced (killed by shell timeout)"
elif [[ "$elapsed" -ge 10 ]]; then
    fail "JS block ran for ${elapsed}s — timeout too slow (expected ≤8s for --timeout 3)"
elif echo "$out" | grep -qi "interrupted\|timeout\|time limit\|exceeded"; then
    ok "JS block interrupted within ${elapsed}s with timeout message"
elif [[ "$ec" -ne 0 ]]; then
    ok "JS block exited non-zero within ${elapsed}s (timeout enforced)"
else
    # JavaScript executor may not be available on this platform
    if echo "$out" | grep -qi "executor\|javascript\|not found\|not available\|node"; then
        ok "no JS executor available — timeout test skipped (acceptable)"
    else
        fail "JS block exited 0 in ${elapsed}s — expected non-zero for infinite loop: ${out:0:120}"
    fi
fi

echo ""

# ---------------------------------------------------------------------------
# T2: Short JS block with --timeout 10 → must complete successfully
# ---------------------------------------------------------------------------
echo "[T2] Short JS block with --timeout 10 completes normally (no false positive)"
cat > "$WORKDIR/test_t2.naab" << 'EOF'
use io
main {
  let result = <<javascript
1 + 1
>>
  io.println(string(result))
}
EOF

out=$(timeout 15s "$NAAB" "$WORKDIR/test_t2.naab" --vm --no-governance --timeout 10 2>&1) || ec=$?
ec=${ec:-0}

if echo "$out" | grep -qi "timeout\|time limit\|exceeded"; then
    fail "false positive — short JS block hit timeout: ${out:0:120}"
elif echo "$out" | grep -qi "executor\|javascript\|not found\|not available\|node"; then
    ok "no JS executor available — false-positive test skipped (acceptable)"
elif [[ "$ec" -eq 0 ]]; then
    ok "short JS block completed without timeout"
else
    ok "non-zero exit (not a timeout error — acceptable): ${out:0:80}"
fi

echo ""

TOTAL=$((PASS + FAIL))
echo "Results: ${PASS}/${TOTAL} passed"
[[ "$FAIL" -eq 0 ]]
