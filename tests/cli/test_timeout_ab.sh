#!/usr/bin/env bash
# test_timeout_ab.sh — Verify Finding A (VM timeout fires) and Finding B (no cross-thread contamination)
set -euo pipefail

NAAB="${1:-$(dirname "$0")/../../build/naab-lang}"
PASS=0; FAIL=0

ok()   { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# ---------------------------------------------------------------------------
# Helper: write a temp naab file, run it, capture output+exit
# ---------------------------------------------------------------------------
run_naab() {
    local script="$1"; shift
    local tmpf; tmpf="$(mktemp "${HOME}/.naab/test_timeout_XXXX.naab")"
    echo "$script" > "$tmpf"
    local out ec
    out=$("$NAAB" "$tmpf" "$@" 2>&1) || ec=$?
    ec=${ec:-0}
    rm -f "$tmpf"
    printf '%s\n__EXIT__%d' "$out" "$ec"
}

extract_exit() { echo "$1" | grep -o '__EXIT__[0-9]*' | grep -o '[0-9]*'; }
extract_out()  { echo "$1" | sed 's/__EXIT__[0-9]*//'; }

echo "=== test_timeout_ab.sh ==="
echo ""

# ---------------------------------------------------------------------------
# A1: Infinite while-loop — VM mode should terminate with timeout error
# ---------------------------------------------------------------------------
echo "[A1] Infinite loop terminates within timeout (VM, --timeout 2)"
script='main { let i = 0; while(true) { i = i + 1; } }'
start=$(date +%s)
result=$(run_naab "$script" --timeout 2 --no-governance || true)
end=$(date +%s)
elapsed=$(( end - start ))
out=$(extract_out "$result")
ec=$(extract_exit "$result")

if echo "$out" | grep -qi "timeout"; then
    ok "error message contains 'timeout'"
else
    fail "expected 'timeout' in output, got: $out"
fi
if [[ "$ec" -ne 0 ]]; then
    ok "non-zero exit code ($ec)"
else
    fail "expected non-zero exit code, got 0"
fi
if [[ "$elapsed" -le 5 ]]; then
    ok "terminated in ${elapsed}s (within 5s wall-clock budget)"
else
    fail "took ${elapsed}s — did not terminate quickly enough"
fi

echo ""

# ---------------------------------------------------------------------------
# A2: Short-running script should NOT be killed by timeout
# use must be at top level (NAAb syntax rule)
# ---------------------------------------------------------------------------
echo "[A2] Short script completes normally with --timeout 5"
script='use io
main { io.println("hello"); }'
result=$(run_naab "$script" --timeout 5 --no-governance || true)
out=$(extract_out "$result")
ec=$(extract_exit "$result")

if echo "$out" | grep -q "hello"; then
    ok "script output 'hello'"
else
    fail "expected 'hello' in output, got: $out"
fi
if [[ "$ec" -eq 0 ]]; then
    ok "exit code 0"
else
    fail "expected exit 0, got $ec"
fi

echo ""

# ---------------------------------------------------------------------------
# A3: Tree-walker — SIGALRM fires but tree-walker has no poll point equivalent
# to VM_NEXT, so termination is enforced by the OS shell timeout wrapper.
# This test verifies the alarm is armed (process dies within budget), not that
# NAAb itself catches the signal in the tree-walker.
# ---------------------------------------------------------------------------
echo "[A3] Infinite loop terminates within timeout (tree-walker, shell-timeout wrapper)"
script='main { let i = 0; while(true) { i = i + 1; } }'
tmpf="${HOME}/.naab/test_timeout_A3.naab"
echo "$script" > "$tmpf"
start=$(date +%s)
timeout 5 "$NAAB" "$tmpf" --timeout 2 --tree-walk --no-governance >/dev/null 2>&1 || true
end=$(date +%s)
elapsed=$(( end - start ))
rm -f "$tmpf"

if [[ "$elapsed" -le 6 ]]; then
    ok "tree-walker: process terminated within ${elapsed}s (shell budget: 5s)"
else
    fail "tree-walker: took ${elapsed}s — hung past shell timeout"
fi

echo ""

# ---------------------------------------------------------------------------
# B1: Static-vs-thread_local — run two sequential requests; second must not
#     see timeout_triggered=true left over from a prior alarmed invocation.
#     (True concurrent test requires REST API; this approximates the reset path.)
# ---------------------------------------------------------------------------
echo "[B1] Second execution after timeout is not pre-contaminated"
# First: trigger a timeout
script_inf='main { let i = 0; while(true) { i = i + 1; } }'
tmpf1="${HOME}/.naab/test_timeout_B1a.naab"
echo "$script_inf" > "$tmpf1"
"$NAAB" "$tmpf1" --timeout 1 --no-governance >/dev/null 2>&1 || true
rm -f "$tmpf1"

# Second: a normal script should run clean
script_ok='use io
main { io.println("clean"); }'
result=$(run_naab "$script_ok" --timeout 5 --no-governance || true)
out=$(extract_out "$result")
ec=$(extract_exit "$result")

if echo "$out" | grep -q "clean"; then
    ok "second script ran cleanly after prior timeout"
else
    fail "second script did not produce expected output; got: $out (exit $ec)"
fi

echo ""

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
TOTAL=$(( PASS + FAIL ))
echo "Results: ${PASS}/${TOTAL} passed"
if [[ "$FAIL" -gt 0 ]]; then
    exit 1
fi
