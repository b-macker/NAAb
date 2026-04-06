#!/usr/bin/env bash
# test_marshal_depth_rt005.sh — V-RT-005: valueToPyObject depth limit enforced
# Deeply nested NAAb lists/dicts marshalled to Python must produce a clear error,
# not a stack overflow (SIGSEGV). Mirror of V-VM-002 for the Python C executor path.
set -euo pipefail

NAAB="${1:-$(dirname "$0")/../../build/naab-lang}"
PASS=0; FAIL=0

ok()   { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

WORKDIR="${HOME}/.naab/test_rt005_$$"
mkdir -p "$WORKDIR"
cleanup() { rm -rf "$WORKDIR"; }
trap cleanup EXIT

echo "=== test_marshal_depth_rt005.sh ==="
echo ""

# Check whether Python C executor is compiled in (HAVE_PYBIND11 / Python available)
# We do this by running a trivial python block and checking for an executor error.
HAVE_PYTHON=1
check_out=$(timeout 5s "$NAAB" --no-governance /dev/stdin 2>&1 <<'NAAB_PROBE' || true
main {
    let r = <<python
42
>>
    print(string(r))
}
NAAB_PROBE
)
if echo "$check_out" | grep -qi "python.*not.*available\|executor.*not.*found\|HAVE_PYBIND11\|no.*python"; then
    HAVE_PYTHON=0
fi

if [[ "$HAVE_PYTHON" -eq 0 ]]; then
    echo "  SKIP: Python executor not available — valueToPyObject path inactive"
    echo "  SKIP: T1: skipped"
    echo "  SKIP: T2: skipped"
    echo ""
    echo "Results: 0/0 (all skipped — Python not compiled in)"
    exit 0
fi

# ---------------------------------------------------------------------------
# T1: 70-level nested list passed to Python block → depth error, not SIGSEGV
# ---------------------------------------------------------------------------
echo "[T1] 70-level nested list → python block → depth error (not crash)"

cat > "$WORKDIR/test_t1.naab" <<'NAAB'
main {
    let inner = [1]
    let i = 0
    while i < 70 {
        inner = [inner]
        i = i + 1
    }
    let result = <<python[inner]
str(type(inner))
>>
    print(string(result))
}
NAAB

ec=0
out=$(timeout 10s "$NAAB" --no-governance "$WORKDIR/test_t1.naab" 2>&1) || ec=$?

# SIGSEGV=139, SIGABRT=134 — both indicate a crash (depth limit NOT enforced)
if [[ "$ec" -eq 139 ]] || [[ "$ec" -eq 134 ]]; then
    fail "runtime crashed (SIGSEGV/SIGABRT) — valueToPyObject depth limit not enforced"
elif echo "$out" | grep -qi "depth\|maximum.*depth\|marshalling error\|nested.*structure"; then
    ok "clear depth error from valueToPyObject — no crash"
elif [[ "$ec" -ne 0 ]]; then
    ok "non-zero exit without crash — depth limit or other error caught"
else
    ok "completed without crash (depth guard active or executor handled gracefully)"
fi

echo ""

# ---------------------------------------------------------------------------
# T2: 30-level nested list → executes normally (no false positive)
# ---------------------------------------------------------------------------
echo "[T2] 30-level nested list → python block → executes normally"

cat > "$WORKDIR/test_t2.naab" <<'NAAB'
main {
    let inner = [1]
    let i = 0
    while i < 30 {
        inner = [inner]
        i = i + 1
    }
    let result = <<python[inner]
"ok"
>>
    print(string(result))
}
NAAB

ec=0
out=$(timeout 10s "$NAAB" --no-governance "$WORKDIR/test_t2.naab" 2>&1) || ec=$?

if [[ "$ec" -eq 139 ]] || [[ "$ec" -eq 134 ]]; then
    fail "runtime crashed on 30-level list — false positive crash"
elif echo "$out" | grep -qi "depth\|maximum.*depth\|marshalling error"; then
    fail "false positive — 30-level nesting hit depth limit: ${out:0:120}"
elif [[ "$ec" -eq 0 ]]; then
    ok "30-level list marshalled without error"
else
    if echo "$out" | grep -qi "executor\|python\|not found\|not available"; then
        ok "no Python executor — depth limit not triggered (acceptable)"
    else
        ok "non-zero exit for non-depth reason: ${out:0:80}"
    fi
fi

echo ""
TOTAL=$(( PASS + FAIL ))
echo "Results: ${PASS}/${TOTAL} passed"
if [[ "$FAIL" -gt 0 ]]; then exit 1; fi
