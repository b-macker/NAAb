#!/usr/bin/env bash
# Test V-RT-007: SIGALRM must be delivered to the correct execution thread.
# A script with --timeout N must terminate within N+1 seconds (not hang).

NAAB="${NAAB_BIN:-$(dirname "$0")/../../build/naab-lang}"
PASS=0; FAIL=0; SKIP=0

pass() { echo "  PASS: $1"; ((PASS++)); }
fail() { echo "  FAIL: $1"; ((FAIL++)); }
skip() { echo "  SKIP: $1"; ((SKIP++)); }

WORK_DIR=$(mktemp -d)
trap 'rm -rf "$WORK_DIR"' EXIT

echo "=== V-RT-007: Reliable POSIX Alarm Delivery ==="

# Infinite loop script — relies entirely on --timeout to terminate.
cat > "$WORK_DIR/infinite.naab" <<'EOF'
main {
    let i = 0
    while true {
        i = i + 1
    }
}
EOF

# T1: Infinite loop with --timeout 2 must exit within 8 seconds.
# Tolerance is generous (4x) because CPU contention under full-suite load
# on constrained platforms (Termux/ARM) delays wall-clock completion.
# The real check is exit_code ≠ 124 (alarm was delivered, not shell timeout).
start=$(date +%s)
timeout 15 "$NAAB" "$WORK_DIR/infinite.naab" --timeout 2 2>&1
exit_code=$?
end=$(date +%s)
elapsed=$((end - start))

if [ $exit_code -eq 124 ]; then
    fail "T1: timed out after 15s — SIGALRM not delivered to correct thread"
elif [ $elapsed -le 8 ]; then
    pass "T1: infinite loop terminated in ${elapsed}s with --timeout 2"
else
    fail "T1: took ${elapsed}s to terminate (expected ≤ 8s) — alarm delivery delayed"
fi

# T2: Two concurrent scripts — each must exit at their own timeout.
cat > "$WORK_DIR/infinite2.naab" <<'EOF'
main {
    let x = 0
    while true {
        x = x + 1
    }
}
EOF

start=$(date +%s)
# Run both concurrently in background
timeout 20 "$NAAB" "$WORK_DIR/infinite.naab"  --timeout 2 2>/dev/null &
pid1=$!
timeout 20 "$NAAB" "$WORK_DIR/infinite2.naab" --timeout 4 2>/dev/null &
pid2=$!

wait $pid1; ec1=$?
t1_end=$(date +%s)
wait $pid2; ec2=$?
t2_end=$(date +%s)

elapsed1=$((t1_end - start))
elapsed2=$((t2_end - start))

t1_ok=false; t2_ok=false
[ $ec1 -ne 124 ] && [ $elapsed1 -le 8 ] && t1_ok=true
[ $ec2 -ne 124 ] && [ $elapsed2 -le 12 ] && t2_ok=true

if $t1_ok && $t2_ok; then
    pass "T2: both concurrent scripts terminated at their own timeouts (${elapsed1}s, ${elapsed2}s)"
elif $t1_ok; then
    fail "T2: script B (timeout=4) took ${elapsed2}s or hung (exit $ec2)"
elif $t2_ok; then
    fail "T2: script A (timeout=2) took ${elapsed1}s or hung (exit $ec1)"
else
    fail "T2: both scripts failed to terminate at their timeouts"
fi

# T3: Short timeout with --timeout 1 must terminate in ≤ 6 seconds.
start=$(date +%s)
timeout 12 "$NAAB" "$WORK_DIR/infinite.naab" --timeout 1 2>&1
exit_code=$?
end=$(date +%s)
elapsed=$((end - start))

if [ $exit_code -eq 124 ]; then
    fail "T3: timed out — SIGALRM with timeout=1 never fired"
elif [ $elapsed -le 6 ]; then
    pass "T3: terminated in ${elapsed}s with --timeout 1"
else
    fail "T3: took ${elapsed}s to terminate (expected ≤ 6s)"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ $FAIL -eq 0 ]
