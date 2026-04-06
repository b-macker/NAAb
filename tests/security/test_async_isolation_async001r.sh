#!/usr/bin/env bash
# test_async_isolation_async001r.sh — V-ASYNC-001r: one script timing out must not kill
# concurrent/subsequent scripts via process-wide global_shutdown_.
set -euo pipefail

NAAB="${1:-$(dirname "$0")/../../build/naab-lang}"
PASS=0; FAIL=0

ok()   { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

WORK_DIR="${HOME}/.naab/async001r_$$"
mkdir -p "$WORK_DIR"
cleanup() { rm -rf "$WORK_DIR"; }
trap cleanup EXIT

echo "=== test_async_isolation_async001r.sh ==="
echo ""

# ---------------------------------------------------------------------------
# T1: Script A times out; Script B (run sequentially after) must still work.
#     Before fix: global_shutdown_ left true → Script B exits immediately on
#     first VM_NEXT even though it has full budget. After fix: only
#     timeout_triggered_ (thread-local) is set, cleared by setExecutionTimeout().
# ---------------------------------------------------------------------------
echo "[T1] Script A timeout does not contaminate subsequent Script B"

# Script A: infinite loop → times out
cat > "$WORK_DIR/script_a.naab" <<'NAAB'
main {
    let i = 0
    while true {
        i = i + 1
    }
}
NAAB

# Script B: simple math → must complete and print the result
cat > "$WORK_DIR/script_b.naab" <<'NAAB'
main {
    let x = 6 * 7
    print(string(x))
}
NAAB

# Run Script A with a 2s timeout (it will time out)
ec_a=0
"$NAAB" "$WORK_DIR/script_a.naab" --no-governance --timeout 2 >/dev/null 2>&1 || ec_a=$?

# Script A must have exited non-zero (timeout)
if [[ "$ec_a" -eq 0 ]]; then
    fail "Script A should have timed out but exited 0"
fi

# Now run Script B — must succeed despite Script A's timeout
ec_b=0
out_b=$("$NAAB" "$WORK_DIR/script_b.naab" --no-governance --timeout 10 2>&1) || ec_b=$?

if [[ "$ec_b" -eq 0 ]] && echo "$out_b" | grep -q "42"; then
    ok "Script B completed normally after Script A timeout (no contamination)"
elif echo "$out_b" | grep -qi "timeout\|time.limit\|execution.*limit"; then
    fail "Script B killed by contaminated global_shutdown_: ${out_b:0:120}"
else
    fail "Script B failed for unexpected reason (exit $ec_b): ${out_b:0:120}"
fi

echo ""

# ---------------------------------------------------------------------------
# T2: Verify flag is properly cleared — third script after two timeouts works.
# ---------------------------------------------------------------------------
echo "[T2] Third script succeeds after two prior timeouts"

cat > "$WORK_DIR/script_c.naab" <<'NAAB'
main {
    print("alive")
}
NAAB

# Two timeouts
"$NAAB" "$WORK_DIR/script_a.naab" --no-governance --timeout 1 >/dev/null 2>&1 || true
"$NAAB" "$WORK_DIR/script_a.naab" --no-governance --timeout 1 >/dev/null 2>&1 || true

ec_c=0
out_c=$("$NAAB" "$WORK_DIR/script_c.naab" --no-governance --timeout 10 2>&1) || ec_c=$?

if [[ "$ec_c" -eq 0 ]] && echo "$out_c" | grep -q "alive"; then
    ok "Script C alive after two prior timeouts"
elif echo "$out_c" | grep -qi "timeout\|time.limit"; then
    fail "Script C killed by residual global_shutdown_: ${out_c:0:120}"
else
    fail "Script C failed (exit $ec_c): ${out_c:0:120}"
fi

echo ""
TOTAL=$(( PASS + FAIL ))
echo "Results: ${PASS}/${TOTAL} passed"
if [[ "$FAIL" -gt 0 ]]; then exit 1; fi
