#!/usr/bin/env bash
# ============================================================
# test_coherence_reconcile.sh — every analyzed CDD_TURN row reconciles from
# telemetry alone:
#
#   coherence = previous analyzed coherence
#               - signal penalties            (penalties_detail name=value)
#               + validation_recovery         (penalties_detail name=+value)
#               + sum(coherence_adjustments)  (temporal_decay=-, natural_healing=+,
#                                              floor_absorbed=+, recovery=+)
#
# Found in the repo-sentinel dogfood (F-008): natural healing moved coherence
# on every damaging turn and was reported nowhere, so the listed penalties
# never matched the drop and CDD was reported as broken. Decay, the clamp at 0
# and step-up/pipeline recovery were equally silent.
#
# RC-01  every analyzed row reconciles (the claim)
# RC-02  the run actually exercised each adjustment kind -- without it RC-01
#        passes on a run where nothing but penalties ever moved coherence
# RC-03  negative control: ignoring coherence_adjustments must break the
#        reconciliation, or the field is decorative
# RC-04  penalties_detail carries no adjustment keys -- consumers read a
#        non-empty penalties_detail as "a signal paid this turn"
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"
_SYSTMP="${TMPDIR:-/tmp}"
TEST_TMP="${_SYSTMP}/coherence-reconcile-$$"

PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo "  PASS [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo "  SKIP [$1] $2"; }

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_coherence_reconcile.sh"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
STUB_PID=""
cleanup() {
    [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null
    teardown_isolated_trust
    [ -n "${KEEP_TMP:-}" ] && { echo "kept: $TEST_TMP"; return; }
    rm -rf "${TEST_TMP:?}"
}
trap cleanup EXIT
mkdir -p "$TEST_TMP"
export FAKE_KEY_RECON="fake-key-reconcile"
source "$SCRIPT_DIR/../helpers/stub_launch.sh"

echo "=== coherence reconciles from CDD_TURN telemetry ==="

WDIR="$TEST_TMP/run"; mkdir -p "$WDIR"
# 10 distinct on-topic responses, then a 500 (the failing pipeline stage), then
# two more. Distinct content keeps S21 quiet; S22 does the damage.
python3 - "$WDIR/fixture.json" <<'EOF'
import json, sys
ops = ["add", "subtract", "multiply", "divide", "modulo", "power",
       "minimum", "maximum", "average", "median"]
r = [{"content": f"def {o}(a, b): pass  # implemented {o} operation for the calculator",
      "output_tokens": 30} for o in ops]
r.append({"status": 500, "error": "internal"})
r += [{"content": "def clamp(x, lo, hi): pass  # implemented clamp operation", "output_tokens": 30},
      {"content": "def sign(x): pass  # implemented sign operation", "output_tokens": 30}]
json.dump({"responses": r}, open(sys.argv[1], "w"))
EOF

start_stub "$WDIR/fixture.json" "$WDIR" || { skip "RC-00" "stub failed to start (UNMEASURABLE)"; STUB_PORT=0; }
if [ "$STUB_PORT" != "0" ]; then
cat > "$WDIR/govern.json" <<EOF
{
  "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl" },
  "behavioral_sequences": { "enabled": true },
  "context_drift": { "enabled": true, "level": "advisory", "check_interval_turns": 1,
      "coherence_natural_healing": 0.03,
      "temporal_decay_enabled": true, "temporal_decay_per_minute": 0.5,
      "temporal_decay_grace_minutes": 0 },
  "agents": { "developer": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT",
      "api_key_env": "FAKE_KEY_RECON", "max_tokens": 100, "max_turns": 30,
      "retry": { "max_attempts": 1, "backoff_ms": 0 } } }
}
EOF
# 10 failing validations drive coherence through the floor (S22 is flat and
# never baseline-absorbed); the failing pipeline stage triggers recovery; a
# final pass after failures earns validation_recovery.
cat > "$WDIR/test.naab" <<'EOF'
use agent
main {
    let h = agent.create("developer")
    let i = 0
    while i < 10 {
        let r = agent.send(h, "implement the next calculator operation")
        let _ = agent.record_validation(h, false)
        i = i + 1
    }
    try {
        let p = agent.pipeline([h], "implement the next calculator operation")
    } catch (e) {
        print("STAGE_FAILED")
    }
    let r2 = agent.send(h, "implement clamp")
    let _ = agent.record_validation(h, true)
    let r3 = agent.send(h, "implement sign")
    print("RUN_DONE")
}
EOF
OUT="$(cd "$WDIR" && timeout 120 "$NAAB" test.naab 2>&1)"; RC=$?
if [[ "$OUT" != *RUN_DONE* ]]; then
    fail "RC-00" "fixture program did not complete (rc=$RC) -- every verdict below would be void" "$(tail -5 <<<"$OUT")"
else
    RES="$(python3 - "$WDIR/tele.jsonl" <<'EOF'
import json, re, sys
rows = []
with open(sys.argv[1], encoding="utf-8", errors="replace") as f:
    for line in f:
        try: e = json.loads(line)
        except ValueError: continue
        if e.get("event_type") == "CDD_TURN" and e.get("analyzed") == "true":
            rows.append(e)
ADJ = ("temporal_decay", "natural_healing", "floor_absorbed", "recovery")
def terms(s):
    for part in filter(None, (s or "").split(",")):
        k, _, v = part.partition("=")
        yield k, v
def run(use_adj):
    prev, worst, bad = {}, 0.0, 0
    for e in rows:
        h = e["handle_id"]; c = float(e["coherence"])
        exp = prev.get(h, 1.0)
        for k, v in terms(e.get("penalties_detail")):
            exp += float(v) if v.startswith("+") else -float(v)
        if use_adj:
            for k, v in terms(e.get("coherence_adjustments")):
                exp += float(v)
        # the previous row's coherence is itself rounded to 4 places, so the
        # comparison is against the printed value, not an exact float
        d = abs(exp - c); worst = max(worst, d)
        if d > 0.0006: bad += 1
        prev[h] = c
    return worst, bad
w1, b1 = run(True); w0, b0 = run(False)
kinds = set()
leak = 0
for e in rows:
    kinds |= {k for k, _ in terms(e.get("coherence_adjustments"))}
    if any(k in ADJ for k, _ in terms(e.get("penalties_detail"))): leak += 1
    if any(k == "validation_recovery" for k, _ in terms(e.get("penalties_detail"))):
        kinds.add("validation_recovery")
print(f"rows={len(rows)} worst={w1:.5f} bad={b1} worst_noadj={w0:.5f} bad_noadj={b0} "
      f"kinds={','.join(sorted(kinds))} leak={leak}")
EOF
)"
    echo "  $RES"
    rows=$(sed -n 's/.*rows=\([0-9]*\).*/\1/p' <<<"$RES")
    bad=$(sed -n 's/.* bad=\([0-9]*\) .*/\1/p' <<<"$RES")
    bad0=$(sed -n 's/.*bad_noadj=\([0-9]*\).*/\1/p' <<<"$RES")
    kinds=$(sed -n 's/.*kinds=\([^ ]*\).*/\1/p' <<<"$RES")
    leak=$(sed -n 's/.*leak=\([0-9]*\).*/\1/p' <<<"$RES")

    if [ "${rows:-0}" -ge 10 ] && [ "${bad:-1}" -eq 0 ]; then
        pass "RC-01" "all $rows analyzed rows reconcile from telemetry"
    else
        fail "RC-01" "coherence does not reconcile ($bad of $rows rows off)" "$RES"
    fi
    missing=""
    for k in natural_healing floor_absorbed recovery temporal_decay validation_recovery; do
        [[ ",$kinds," == *",$k,"* ]] || missing="$missing $k"
    done
    if [ -z "$missing" ]; then
        pass "RC-02" "run exercised every adjustment kind ($kinds)"
    else
        fail "RC-02" "run never exercised:$missing -- RC-01 is not covering them"
    fi
    if [ "${bad0:-0}" -gt 0 ]; then
        pass "RC-03" "negative control: without coherence_adjustments $bad0 rows fail to reconcile"
    else
        fail "RC-03" "ignoring coherence_adjustments still reconciles -- the field is not load-bearing here"
    fi
    if [ "${leak:-1}" -eq 0 ]; then
        pass "RC-04" "penalties_detail carries signal penalties only"
    else
        fail "RC-04" "adjustment keys leaked into penalties_detail on $leak rows"
    fi
fi
fi

echo ""
echo "=== the response after a failed API call is still analyzed ==="
# Writing RC-01 found this: a retry-exhausted failure was analysed at the same
# turn number the next real response carries, so that response hit the interval
# check and was never scored -- while its CDD_TURN said analyzed:"true".
# The probe: after the failure, repeat an earlier response verbatim. S21
# response_repetition is objective (never baseline-absorbed), so it MUST fire
# on that response if the response was analysed at all.
#   IA-01/02  exclude_infrastructure_errors true (default) / false
#   IA-03     control: the same repeat with NO failure before it fires S21 --
#             without it IA-01/02 would pass on a fixture that cannot fire
for arm in default noexclude control; do
    AD="$TEST_TMP/ia-$arm"; mkdir -p "$AD"
    python3 - "$AD/fixture.json" "$arm" <<'EOF'
import json, sys
a = {"content": "def add(a, b): return a + b  # implemented add operation", "output_tokens": 30}
b = {"content": "def subtract(a, b): return a - b  # implemented subtract operation", "output_tokens": 30}
r = [a, b] + ([] if sys.argv[2] == "control" else [{"status": 500, "error": "internal"}]) + [dict(a)]
json.dump({"responses": r}, open(sys.argv[1], "w"))
EOF
    STUB_PID=""; start_stub "$AD/fixture.json" "$AD" || { skip "IA-$arm" "stub failed to start (UNMEASURABLE)"; continue; }
    excl=true; [ "$arm" = noexclude ] && excl=false
    cat > "$AD/govern.json" <<EOF
{
  "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl" },
  "behavioral_sequences": { "enabled": true },
  "context_drift": { "enabled": true, "level": "advisory", "check_interval_turns": 1,
      "signals": { "exclude_infrastructure_errors": $excl } },
  "agents": { "developer": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT",
      "api_key_env": "FAKE_KEY_RECON", "max_tokens": 100, "max_turns": 30,
      "retry": { "max_attempts": 1, "backoff_ms": 0 } } }
}
EOF
    if [ "$arm" = control ]; then
        FAILSEND=''
    else
        FAILSEND='    try { let f = agent.send(h, "implement multiply") } catch (e) { print("SEND_FAILED") }'
    fi
    cat > "$AD/test.naab" <<EOF
use agent
main {
    let h = agent.create("developer")
    let r1 = agent.send(h, "implement add")
    let r2 = agent.send(h, "implement subtract")
$FAILSEND
    let r4 = agent.send(h, "implement add again")
    print("RUN_DONE")
}
EOF
    OUT="$(cd "$AD" && timeout 60 "$NAAB" test.naab 2>&1)"
    kill "$STUB_PID" 2>/dev/null; STUB_PID=""
    id=IA-01; [ "$arm" = noexclude ] && id=IA-02; [ "$arm" = control ] && id=IA-03
    if [[ "$OUT" != *RUN_DONE* ]] || { [ "$arm" != control ] && [[ "$OUT" != *SEND_FAILED* ]]; }; then
        fail "$id" "fixture did not run as designed ($arm) -- verdict void" "$(tail -3 <<<"$OUT")"; continue
    fi
    # The LAST CDD_TURN is the repeated response's row.
    LAST="$(python3 - "$AD/tele.jsonl" <<'EOF'
import json, sys
rows = [json.loads(l) for l in open(sys.argv[1], encoding="utf-8", errors="replace")
        if '"CDD_TURN"' in l]
e = rows[-1] if rows else {}
fired = "response_repetition" in (e.get("signals_detail") or "")
print(f"analyzed={e.get('analyzed')} s21={'yes' if fired else 'no'} signals={e.get('signals_detail')}")
EOF
)"
    if [[ "$LAST" == *"analyzed=true s21=yes"* ]]; then
        pass "$id" "$arm: the repeated response was analyzed and S21 fired"
    else
        fail "$id" "$arm: the repeated response escaped CDD" "$LAST"
    fi
done

echo ""
echo "Results: $PASS_COUNT passed, $FAIL_COUNT failed, $SKIP_COUNT skipped"
[ "$FAIL_COUNT" -eq 0 ]
