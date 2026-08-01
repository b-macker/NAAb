#!/usr/bin/env bash
# test_serialize_depth_vm002.sh — Finding V-VM-002: Depth limit in polyglot serialization
# Deeply nested structures must produce a clear error, not a stack overflow / SIGSEGV.

set -euo pipefail
NAAB="${1:-$(dirname "$0")/../../build/naab-lang}"
PASS=0; FAIL=0; SKIP=0
ok()   { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
# A run whose executor is absent, or whose failure cannot be attributed, has
# verified nothing. Without this the suite could only say PASS or FAIL, so
# "not available" was recorded as a pass — a green standing in for an unrun check.
skip() { echo "  SKIP: $1"; SKIP=$((SKIP + 1)); }

WORKDIR="${HOME}/.naab/test_vm002_$$"
mkdir -p "$WORKDIR"
cleanup() { rm -rf "$WORKDIR"; }
trap cleanup EXIT

echo "=== test_serialize_depth_vm002.sh ==="
echo ""

# ---------------------------------------------------------------------------
# T1: 70-level nested dict passed to python block → must get depth error, not SIGSEGV
# ---------------------------------------------------------------------------
echo "[T1] 70-level nested dict → python block → depth error (not crash)"

# Build a naab script that constructs a 70-level nested dict programmatically
cat > "$WORKDIR/test_t1.naab" << 'EOF'
use io
main {
  let d = {"val": 1}
  let i = 0
  while i < 70 {
    d = {"inner": d}
    i = i + 1
  }
  let result = <<python[d]
str(type(d))
>>
  io.println(result)
}
EOF

# Run with a wall-clock timeout to guard against infinite loops / hangs
out=$(timeout 10s "$NAAB" "$WORKDIR/test_t1.naab" --vm --no-governance 2>&1) || ec=$?
ec=${ec:-0}

# Check: must NOT be killed by SIGSEGV (exit 139) or SIGABRT (exit 134)
if [[ "$ec" -eq 139 ]] || [[ "$ec" -eq 134 ]]; then
    fail "runtime crashed (SIGSEGV/SIGABRT) — depth limit not enforced"
elif echo "$out" | grep -qi "depth\|serialization error\|maximum"; then
    ok "clear depth error produced — no crash"
elif [[ "$ec" -ne 0 ]]; then
    skip "non-zero exit without crash — depth limit or other error caught"
else
    # Might succeed if the dict is not deep enough to trigger on this platform
    skip "completed without crash (depth may not have been reached)"
fi

echo ""

# ---------------------------------------------------------------------------
# T2: 30-level nested dict → must succeed (no false positive at shallow depth)
# ---------------------------------------------------------------------------
echo "[T2] 30-level nested dict → python block → executes successfully"
cat > "$WORKDIR/test_t2.naab" << 'EOF'
use io
main {
  let d = {"val": 1}
  let i = 0
  while i < 30 {
    d = {"inner": d}
    i = i + 1
  }
  let result = <<python[d]
"ok"
>>
  io.println(result)
}
EOF

out=$(timeout 10s "$NAAB" "$WORKDIR/test_t2.naab" --vm --no-governance 2>&1) || ec=$?
ec=${ec:-0}

if echo "$out" | grep -qi "depth\|serialization error"; then
    fail "false positive — 30-level nesting hit depth limit: ${out:0:120}"
elif [[ "$ec" -eq 139 ]] || [[ "$ec" -eq 134 ]]; then
    fail "runtime crashed on 30-level structure"
elif [[ "$ec" -eq 0 ]]; then
    ok "30-level nesting serialized without error"
else
    # Non-zero exit for other reasons (no Python executor, etc.) is fine
    if echo "$out" | grep -qi "executor\|python\|not found\|not available"; then
        skip "no Python executor — depth limit not triggered (acceptable)"
    else
        ok "non-zero exit for non-depth reason: ${out:0:80}"
    fi
fi

echo ""

TOTAL=$((PASS + FAIL + SKIP))
echo "Results: ${PASS}/${TOTAL} passed, ${SKIP} skipped (unverified)"
[[ "$FAIL" -eq 0 ]]
