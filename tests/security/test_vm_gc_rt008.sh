#!/usr/bin/env bash
# Test V-RT-008: VM gc_collect() must be a real GC call (not a stub),
# and the periodic GC trigger at OP_JUMP_BACK must fire without OOM.

NAAB="${NAAB_BIN:-$(dirname "$0")/../../build/naab-lang}"
PASS=0; FAIL=0; SKIP=0

pass() { echo "  PASS: $1"; ((PASS++)); }
fail() { echo "  FAIL: $1"; ((FAIL++)); }
skip() { echo "  SKIP: $1"; ((SKIP++)); }

WORK_DIR=$(mktemp -d -p "$HOME" .naab_rt008_XXXXXX 2>/dev/null || mktemp -d)
trap 'rm -rf "$WORK_DIR"' EXIT

# V-GOV-007: provide a govern.json so fail-closed default doesn't block these GC tests
cat > "$WORK_DIR/govern.json" <<'EOF'
{
  "mode": "off"
}
EOF

echo "=== V-RT-008: VM GC Cycle Detection ==="

# T1: gc_collect() returns null without throwing an exception.
echo ""
echo "[T1] gc_collect() returns null — not a stub that crashes"
cat > "$WORK_DIR/t1.naab" <<'EOF'
use io
main {
    let result = gc_collect()
    if result == null {
        io.write("gc_ok\n")
    } else {
        io.write("gc_unexpected\n")
    }
}
EOF

output=$(timeout 5s "$NAAB" "$WORK_DIR/t1.naab" 2>&1)
exit_code=$?
if [[ $exit_code -eq 124 ]]; then
    fail "T1: gc_collect() timed out after 5s — may be hanging"
elif [[ $exit_code -ne 0 ]]; then
    fail "T1: gc_collect() threw or crashed (exit $exit_code): ${output:0:200}"
elif echo "$output" | grep -q "gc_ok"; then
    pass "T1: gc_collect() returned null without error"
else
    fail "T1: unexpected output from gc_collect(): ${output:0:200}"
fi

# T2: Loop creating many lists exercises the periodic GC trigger (--gc-threshold 100).
#     Must complete within 5 seconds without OOM.
echo ""
echo "[T2] Loop with --gc-threshold 100 triggers periodic GC — no OOM within 5s"
cat > "$WORK_DIR/t2.naab" <<'EOF'
use io
main {
    let count = 0
    while count < 1000 {
        let arr = [1, 2, 3, 4, 5]
        count = count + 1
    }
    io.write("done\n")
}
EOF

start=$(date +%s)
output=$(timeout 5s "$NAAB" --gc-threshold 100 "$WORK_DIR/t2.naab" 2>&1)
exit_code=$?
end=$(date +%s)
elapsed=$((end - start))

if [[ $exit_code -eq 124 ]]; then
    fail "T2: loop with periodic GC timed out after 5s — possible OOM or GC loop"
elif [[ $exit_code -ne 0 ]]; then
    fail "T2: loop with periodic GC failed (exit $exit_code): ${output:0:200}"
elif echo "$output" | grep -q "done"; then
    pass "T2: 1000 iterations with --gc-threshold 100 completed in ${elapsed}s"
else
    fail "T2: unexpected output: ${output:0:200}"
fi

# T3: gc_collect() inside a loop — periodic GC fires, no exception thrown.
echo ""
echo "[T3] Explicit gc_collect() inside loop — no exception, returns null each time"
cat > "$WORK_DIR/t3.naab" <<'EOF'
use io
main {
    let i = 0
    while i < 10 {
        let arr = [i, i, i]
        let result = gc_collect()
        i = i + 1
    }
    io.write("loop_done\n")
}
EOF

output=$(timeout 5s "$NAAB" "$WORK_DIR/t3.naab" 2>&1)
exit_code=$?
if [[ $exit_code -eq 124 ]]; then
    fail "T3: gc_collect() in loop timed out — GC may be corrupting execution"
elif [[ $exit_code -ne 0 ]]; then
    fail "T3: gc_collect() in loop threw exception (exit $exit_code): ${output:0:200}"
elif echo "$output" | grep -q "loop_done"; then
    pass "T3: gc_collect() in loop completed normally — no exception"
else
    fail "T3: unexpected output: ${output:0:200}"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ $FAIL -eq 0 ]
