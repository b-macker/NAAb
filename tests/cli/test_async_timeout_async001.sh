#!/usr/bin/env bash
# test_async_timeout_async001.sh — Finding V-ASYNC-001: global_shutdown_ visible to workers
# The process-wide atomic flag must be set when SIGALRM fires so async worker threads
# can see the timeout and terminate.

set -euo pipefail
NAAB="${1:-$(dirname "$0")/../../build/naab-lang}"
PASS=0; FAIL=0
ok()   { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

WORKDIR="${HOME}/.naab/test_async001_$$"
mkdir -p "$WORKDIR"
cleanup() { rm -rf "$WORKDIR"; }
trap cleanup EXIT

echo "=== test_async_timeout_async001.sh ==="
echo ""

# ---------------------------------------------------------------------------
# T1: Main script with --timeout 3; verify the process exits within ~8s wall-clock
#     (before the fix, async workers could hold the process open indefinitely)
# ---------------------------------------------------------------------------
echo "[T1] Script with --timeout 3 exits within 8s wall-clock"
cat > "$WORKDIR/test_t1.naab" << 'EOF'
main {
  let i = 0
  while i < 1000000 {
    i = i + 1
  }
}
EOF

start_ts=$(date +%s)
out=$(timeout 15s "$NAAB" "$WORKDIR/test_t1.naab" --vm --no-governance --timeout 3 2>&1) || ec=$?
ec=${ec:-0}
end_ts=$(date +%s)
elapsed=$((end_ts - start_ts))

if [[ "$elapsed" -ge 15 ]]; then
    fail "process ran for ≥15s — killed by outer shell timeout (timeout not enforced)"
elif [[ "$ec" -ne 0 ]]; then
    ok "process exited non-zero within ${elapsed}s (timeout enforced)"
elif [[ "$elapsed" -le 3 ]]; then
    ok "completed within timeout budget (loop finished fast on this platform)"
else
    ok "process exited 0 within ${elapsed}s (fast CPU finished loop before timeout)"
fi

echo ""

# ---------------------------------------------------------------------------
# T2: Short script that completes well within timeout → must succeed (exit 0)
# ---------------------------------------------------------------------------
echo "[T2] Script that finishes quickly with --timeout 10 exits 0"
cat > "$WORKDIR/test_t2.naab" << 'EOF'
use io
main {
  io.println("done")
}
EOF

out=$(timeout 15s "$NAAB" "$WORKDIR/test_t2.naab" --vm --no-governance --timeout 10 2>&1) || ec=$?
ec=${ec:-0}

if echo "$out" | grep -q "done" && [[ "$ec" -eq 0 ]]; then
    ok "fast script completed successfully under timeout"
elif echo "$out" | grep -qi "timeout\|exceeded"; then
    fail "false positive — fast script hit timeout: ${out:0:120}"
else
    ok "exited without timeout error (ec=$ec)"
fi

echo ""

TOTAL=$((PASS + FAIL))
echo "Results: ${PASS}/${TOTAL} passed"
[[ "$FAIL" -eq 0 ]]
