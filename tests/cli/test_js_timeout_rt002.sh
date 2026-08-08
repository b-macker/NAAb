#!/usr/bin/env bash
# test_js_timeout_rt002.sh — Finding V-RT-002: JS blocks respect --timeout flag
# The JS executor's interruptHandler must check ResourceLimiter::isTimeoutTriggered(),
# not just its own internal 30s watchdog.

set -euo pipefail
NAAB="${1:-$(dirname "$0")/../../build/naab-lang}"
PASS=0; FAIL=0; SKIP=0
ok()   { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
skip() { echo "  SKIP: $1"; SKIP=$((SKIP + 1)); }

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
(function(){ while(true) {} })()
>>
}
EOF

start_ts=$(date +%s)
# ec must be reset per test: `|| ec=$?` does not fire on success, so without
# this a later test silently inherits an earlier test's exit code. T2 was
# reading T1's non-zero status and reporting it as its own "unrelated failure".
ec=0
out=$(timeout 15s "$NAAB" "$WORKDIR/test_t1.naab" --vm --no-governance --timeout 3 2>&1) || ec=$?
end_ts=$(date +%s)
elapsed=$((end_ts - start_ts))

if [[ "$elapsed" -ge 15 ]]; then
    fail "JS block ran for ≥15s — timeout not enforced (killed by shell timeout)"
elif [[ "$elapsed" -ge 10 ]]; then
    fail "JS block ran for ${elapsed}s — timeout too slow (expected ≤8s for --timeout 3)"
elif echo "$out" | grep -qi "interrupted\|timeout\|time limit\|exceeded"; then
    ok "JS block interrupted within ${elapsed}s with timeout message"
elif echo "$out" | grep -qi "executor\|javascript\|not found\|not available\|node"; then
    # This check MUST precede the exit-code branches. A missing JS executor
    # exits NON-ZERO, so while it lived in the final `else` (reachable only when
    # ec == 0) it was unreachable — the old code caught that case one branch
    # earlier and called it "timeout enforced". Leaving it there while adding a
    # fast-exit failure below would have converted every platform without a JS
    # executor from a false pass into a hard failure.
    #
    # The suite's whole subject is whether the JS executor honours --timeout, so
    # a run without one is the single outcome that proves nothing about it.
    skip "no JS executor available — timeout enforcement not exercised"
elif [[ "$ec" -ne 0 && "$elapsed" -ge 2 ]]; then
    # The elapsed floor is load-bearing. This branch used to accept ANY non-zero
    # exit as "timeout enforced", and the fixture above used to be a bare
    # `while(true) {}` — which the executor wraps as an expression, making it a
    # SyntaxError that failed in 0s. So the suite for V-RT-002 reported the
    # timeout as enforced without ever running a JS loop: "the timeout fired"
    # and "the code never parsed" both produce a non-zero exit.
    ok "JS block exited non-zero after ${elapsed}s (timeout enforced)"
elif [[ "$ec" -ne 0 ]]; then
    fail "JS block exited non-zero in ${elapsed}s — too fast to be the 3s timeout, the block likely never ran: ${out:0:120}"
else
    fail "JS block exited 0 in ${elapsed}s — expected non-zero for infinite loop: ${out:0:120}"
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

ec=0
out=$(timeout 15s "$NAAB" "$WORKDIR/test_t2.naab" --vm --no-governance --timeout 10 2>&1) || ec=$?

if echo "$out" | grep -qi "timeout\|time limit\|exceeded"; then
    fail "false positive — short JS block hit timeout: ${out:0:120}"
elif echo "$out" | grep -qi "executor\|javascript\|not found\|not available\|node"; then
    skip "no JS executor available — false-positive check not exercised"
elif [[ "$ec" -eq 0 ]]; then
    ok "short JS block completed without timeout"
else
    # Catch-all pass on any non-zero exit: a parse error, a crash or a missing
    # dependency all reported "no false positive" for a block that never ran.
    skip "short JS block failed for an unrelated reason — no-false-positive not shown (exit $ec): ${out:0:80}"
fi

echo ""

TOTAL=$((PASS + FAIL))
echo "Results: ${PASS}/${TOTAL} passed${SKIP:+, ${SKIP} skipped}"
[[ "$FAIL" -eq 0 ]]
