#!/usr/bin/env bash
# ============================================================
# test_pressure_level_map.sh — what pressure a drifting agent can actually
# generate, and which rungs of the ladder that reaches
#
# WHY THIS EXISTS
#
# The circuit breaker's levels are gated on a composite pressure score, and
# until adaptive baselining defaulted on, coherence — the largest single term —
# was uncalibrated, so any measurement of this mapping was confounded by
# false-killed correct work. With coherence trustworthy the mapping can be
# measured, and it says two things that are arithmetic rather than opinion.
#
# 1. PRESSURE CEILINGS AT 0.70, SO CRITICAL IS UNREACHABLE THROUGH DRIFT.
#
#    Ten factors exist; five ship at weight 0.0 (opt-in). Of the five active:
#
#      coherence_proximity   0.35   clamp(1 - coherence/coherence_threshold)
#      signal_density        0.25   clamp(signals_fired_this_turn / 4.0)
#      risk_score_proximity  0.20   DEAD unless scoring.enabled, default FALSE
#      conversation_depth    0.10   clamp(turn / 20)
#      bsd_partial_progress  0.10   needs a partially-matching BSD pattern
#
#    Behavioural drift moves the first, second and fourth. Their sum is
#    0.35 + 0.25 + 0.10 = 0.70, against critical_threshold 0.80. An agent at
#    coherence 0.0 with every signal firing, 25 turns deep, tops out at exactly
#    0.7000 — measured, and it sits there. CRITICAL requires static-scoring
#    violations or BSD progress, neither of which is behavioural drift.
#
#    This matters because CRITICAL is the only rung with an implemented effect
#    (suspending autonomous actions). ELEVATED and HIGH are reachable and inert
#    (they were, until test_level_effects.sh). A fully incoherent agent lands on a rung
#    that does nothing, and cannot reach the rung that does.
#
# 2. ESCALATION NEEDS COHERENCE AT OR NEAR ZERO. coherence_proximity is
#    weighted 0.35 and clamped to 1.0, so coherence ALONE contributes at most
#    0.35 — below elevated_threshold 0.40. Measured: at coherence 0.3500
#    pressure was 0.3975 and the level was still NORMAL; ELEVATED arrived only
#    once coherence had saturated at 0.0000.
#
# WHAT THIS FILE DOES AND DOES NOT CLAIM
#
# It pins the mapping as measured. It does NOT assert that the ceiling is a
# defect — requiring more than behavioural evidence before suspending an agent
# is a defensible design. It exists so that if the weights, thresholds or
# factor set are changed, the consequence for reachability is visible in a
# diff instead of being rediscovered.
#
# ASSERTIONS
#   A  the sweep is live and decomposes the way the engine says it does
#   B  the reachable rungs fire, in order
#   C  the 0.70 ceiling — reached, and not exceeded, and CRITICAL absent
#   D  POSITIVE CONTROL: CRITICAL fires when its threshold is brought under
#      the ceiling. Without D, group C passes for a build whose escalation is
#      broken outright, which is the opposite conclusion.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/pmap-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_pressure_level_map.sh"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
STUB_PID=""
cleanup() { [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null; teardown_isolated_trust; rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"

"$NAAB" --keygen "$TEST_TMP/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/k.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/k.pem"
export FAKE_KEY_PMAP="fake-key-pressure-map"
sign_govern() { (cd "$1" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true; }
source "$SCRIPT_DIR/../helpers/stub_launch.sh"

# Five CLEAN distinct warm-up turns so the adaptive baseline calibrates on
# clean behaviour (a fixture that drifts from turn 1 teaches the baseline that
# drift is normal and is absorbed for the whole run), then twenty drift turns:
# unrelated topic each time, distinct so response_repetition is not the driver,
# and progressively terser so several signals converge for signal_density.
gen_fixture() {
    python3 - "$1" <<'PYEOF'
import json, sys
r = []
for t in ["parsed", "validated", "grouped", "reconciled", "published"]:
    r.append({"content": "The ledger reconcile job %s the quarterly totals and recorded the balance." % t,
              "output_tokens": 45, "thinking_tokens": 20})
topics = ["photography lenses","mountain weather","pasta recipes","jazz history","bicycle gears",
          "tide tables","volcano types","origami folds","desert beetles","harbour cranes",
          "violin varnish","cave minerals","kite design","tram signalling","reef fishes",
          "clock escapements","dye plants","glacier moraine","seed banks","radio masts"]
for i, t in enumerate(topics):
    r.append({"content": "Consider %s instead." % t,
              "output_tokens": max(6, 30 - i), "thinking_tokens": 0})
json.dump({"responses": r}, open(sys.argv[1], "w"))
PYEOF
}

# $1=port  $2=extra circuit_breaker fields (leading comma) — defaults otherwise
mk_govern() {
cat <<EOF
{
  "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl" },
  "behavioral_sequences": { "enabled": true },
  "context_drift": { "enabled": true, "level": "advisory", "check_interval_turns": 1,
    "adaptive_baseline_window": 5 },
  "circuit_breaker": { "enabled": true${2} },
  "agents": { "worker": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$1",
      "api_key_env": "FAKE_KEY_PMAP", "max_tokens": 200, "max_turns": 40,
      "system_prompt": "You reconcile ledger data and report quarterly totals." } }
}
EOF
}

SCRIPT='use agent
main {
    let h = agent.create("worker")
    let i = 0
    while i < 25 {
        try { let r = agent.send(h, "continue the ledger reconciliation") }
        catch (e) { print("BLOCKED") }
        i = i + 1
    }
    print("DONE")
}'

run_case() {  # $1=workdir $2=extra circuit_breaker json
    WDIR="$TEST_TMP/$1"; mkdir -p "$WDIR"
    gen_fixture "$WDIR/fixture.json"
    start_stub "$WDIR/fixture.json" "$WDIR" || return 1
    mk_govern "$STUB_PORT" "$2" > "$WDIR/govern.json"; sign_govern "$WDIR"
    printf '%s' "$SCRIPT" > "$WDIR/t.naab"
    OUT=$(cd "$WDIR" && timeout 200s "$NAAB" t.naab 2>&1) || true
    stop_stub
    return 0
}

# Emit "turn coherence pressure level detail" for analysed CDD_TURN rows only.
# analyzed=false rows re-show STALE state from the last analysed check; reading
# them as per-turn data is how two earlier forensic passes were misled.
rows() {
    python3 - "$1" <<'PYEOF'
import json, sys
for line in open(sys.argv[1]):
    if '"CDD_TURN"' not in line: continue
    try: e = json.loads(line)
    except Exception: continue
    d = e.get("fields", e)
    if d.get("event_type") != "CDD_TURN" or d.get("analyzed") != "true": continue
    print("%s %s %s %s %s" % (d.get("turn"), d.get("coherence"), d.get("pressure"),
                              d.get("governance_level"), (d.get("pressure_detail") or "-")))
PYEOF
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  Pressure -> level: what drift can actually reach             |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

# ============================================================
echo -e "${CYAN}--- Group A: the sweep is live and decomposes as documented ---${NC}"
if ! run_case base ""; then skip "A-01" "stub failed"; else
R=$(rows "$WDIR/tele.jsonl")
NROWS=$(echo "$R" | grep -c . || true)
MINCOH=$(echo "$R" | awk '{print $2}' | sort -g | head -1)
MAXP=$(echo "$R" | awk '{print $3}' | sort -g | tail -1)
LEVELS=$(echo "$R" | awk '{print $4}' | sort -u | tr '\n' ' ')

if [ "${NROWS:-0}" -ge 20 ]; then
    pass "A-01" "Sweep produced $NROWS analysed turns"
else
    fail "A-01" "Too few analysed turns to map anything" "got $NROWS"
fi
if [ -n "$MINCOH" ] && awk "BEGIN{exit !($MINCOH <= 0.0001)}"; then
    pass "A-02" "Coherence reached the floor ($MINCOH) — the full range is swept"
else
    fail "A-02" "Coherence never reached 0; the top of the range is untested" "min=$MINCOH"
fi
# Only the five active factors may appear. An opt-in factor at weight 0.0
# showing up here means a weight moved without this file being updated.
STRAY=$(echo "$R" | grep -o 'pipeline_inherited=\|coherence_acceleration=\|codegen_pressure=\|bsd_eviction_pressure=\|semantic_deviation=' | sort -u | tr '\n' ' ')
if [ -z "$STRAY" ]; then
    pass "A-03" "Only default-weighted factors contribute (no opt-in terms present)"
else
    fail "A-03" "An opt-in factor is contributing at default weights" "$STRAY"
fi
# coherence_proximity saturates at its weight (0.35) once coherence is 0.
SAT=$(echo "$R" | grep -o 'coherence_prox=[0-9.]*' | sort -u | awk -F= '{print $2}' | sort -g | tail -1)
if [ -n "$SAT" ] && awk "BEGIN{exit !($SAT >= 0.3499 && $SAT <= 0.3501)}"; then
    pass "A-04" "coherence_proximity saturates at its weight (0.35), clamped"
else
    fail "A-04" "coherence_proximity did not saturate at 0.35" "max=$SAT"
fi
fi

# ============================================================
echo -e "${CYAN}--- Group B: the reachable rungs fire, in order ---${NC}"
if [ -n "${R:-}" ]; then
FIRST_ELEV_COH=$(echo "$R" | awk '$4=="elevated"{print $2; exit}')
if echo "$LEVELS" | grep -q elevated; then
    pass "B-01" "ELEVATED reached (first at coherence $FIRST_ELEV_COH)"
else
    fail "B-01" "ELEVATED never reached" "levels seen: $LEVELS"
fi
if echo "$LEVELS" | grep -q high; then
    pass "B-02" "HIGH reached"
else
    fail "B-02" "HIGH never reached" "levels seen: $LEVELS"
fi
# The measured claim: coherence alone is weighted 0.35, below elevated_threshold
# 0.40, so ELEVATED cannot arrive until coherence is at or very near the floor.
if [ -n "$FIRST_ELEV_COH" ] && awk "BEGIN{exit !($FIRST_ELEV_COH <= 0.15)}"; then
    pass "B-03" "ELEVATED requires coherence at/near the floor ($FIRST_ELEV_COH <= 0.15)"
else
    fail "B-03" "ELEVATED arrived at a higher coherence than the weights allow" \
         "first elevated at coherence=$FIRST_ELEV_COH"
fi
fi

# ============================================================
echo -e "${CYAN}--- Group C: the 0.70 ceiling, and CRITICAL out of reach ---${NC}"
if [ -n "${R:-}" ]; then
# Reached — without this, C-02 is satisfied by any run that stays quiet.
if [ -n "$MAXP" ] && awk "BEGIN{exit !($MAXP >= 0.6990)}"; then
    pass "C-01" "Ceiling is actually reached (max pressure $MAXP)"
else
    fail "C-01" "Run never approached the ceiling; C-02 would pass vacuously" "max=$MAXP"
fi
# Not exceeded.
if [ -n "$MAXP" ] && awk "BEGIN{exit !($MAXP <= 0.7001)}"; then
    pass "C-02" "Pressure never exceeds 0.70 under default weights ($MAXP)"
else
    fail "C-02" "Pressure exceeded the documented ceiling" \
         "max=$MAXP — a factor weight or the factor set changed"
fi
if ! echo "$LEVELS" | grep -q critical; then
    pass "C-03" "CRITICAL not reached by drift alone (ceiling 0.70 < threshold 0.80)"
else
    fail "C-03" "CRITICAL reached — the ceiling analysis is stale" "levels: $LEVELS"
fi
fi

# ============================================================
echo -e "${CYAN}--- Group D: POSITIVE CONTROL — CRITICAL under the ceiling ---${NC}"
# C-03 alone cannot distinguish "the ceiling is below the threshold" from
# "escalation is broken and nothing ever fires". Same drift, same weights, with
# critical_threshold moved beneath the measured ceiling: CRITICAL must fire.
if ! run_case critlow ', "critical_threshold": 0.65, "critical_sustained": 2'; then
    skip "D-01" "stub failed"
else
R2=$(rows "$WDIR/tele.jsonl")
LEVELS2=$(echo "$R2" | awk '{print $4}' | sort -u | tr '\n' ' ')
CRIT_TELE=$(grep -c '"to_level":"critical"' "$WDIR/tele.jsonl" 2>/dev/null || true)
if echo "$LEVELS2" | grep -q critical || [ "${CRIT_TELE:-0}" -ge 1 ]; then
    pass "D-01" "CRITICAL fires once its threshold is under the ceiling (control)"
else
    fail "D-01" "CRITICAL unreachable even below the ceiling — escalation is broken" \
         "levels: $LEVELS2 — C-03 proves nothing without this"
fi
fi

# ============================================================
echo ""
echo -e "${CYAN}+==============================================================+${NC}"
TOTAL=$((PASS_COUNT + FAIL_COUNT + SKIP_COUNT))
echo -e "  Total: $TOTAL | ${GREEN}Pass: $PASS_COUNT${NC} | ${RED}Fail: $FAIL_COUNT${NC} | ${YELLOW}Skip: $SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then echo -e "${RED}Failures:${NC}$FAILURES"; exit 1; fi
exit 0
