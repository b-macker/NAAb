#!/usr/bin/env bash
# ============================================================
# test_deescalation_reachability.sh — once coherence floors, the level does not
# come back down
#
# THE FINDING
#
# coherence_natural_healing defaults to 0.0, so coherence is monotonically
# non-increasing: nothing raises it except an explicit recovery channel. The
# composite pressure that drives the circuit breaker weights
# coherence_proximity at 0.35, clamped:
#
#     coherence_prox = clamp(1 - coherence / coherence_threshold, 0, 1)
#
# At coherence 0 that term pins at its maximum and contributes a flat 0.35
# forever. conversation_depth adds up to a further 0.10, so from roughly turn 10
# onward pressure sits at ~0.45 against elevated_threshold 0.40 — above it, with
# no signal firing at all. The de-escalation branch is never even entered: the
# calm counter stays at 0 because `target >= prev_level` on every turn.
#
# So a floored agent that goes completely quiet stays at ELEVATED for the rest
# of the run. Not because de-escalation is broken — DR-04 shows it working the
# moment the coherence term is neutralised — but because the input that would
# trigger it can no longer move.
#
# A FIRST DRAFT OF THIS FILE GOT THE CONTROL WRONG, which is worth recording
# because the wrong control looked more natural. It tried "a run whose coherence
# never floors", and that arm did not de-escalate either — so the assertion pair
# would have been misattributed to flooring. The reason is the same term: with
# coherence_threshold at 0.7, reaching elevated_threshold 0.40 at all requires
# enough signals to drag coherence well below 0.7, at which point coherence_prox
# is already live and, with healing at its default 0, one-way. Escalating and
# keeping coherence high are close to mutually exclusive on a stock config.
#
# WHY THIS IS WORTH A TEST RATHER THAN A DOC LINE
#
# It is a precondition for other work. An attempt to make escalation
# effectiveness inform de-escalation (hold the step-down when the last
# escalation measured ineffective) was written, built, and REVERTED unshipped:
# it was correct in design and unreachable in practice, because the decision it
# hooks into does not run in the state where it would matter. Four A/B probes
# at multiplier 1 vs 2 produced byte-identical output. Anything else that plans
# to consume the de-escalation path needs to know this first.
#
# This file asserts the CURRENT behaviour. It is not a claim that the behaviour
# is wrong — permanently elevated scrutiny for an agent whose coherence has
# collapsed is defensible. It is a claim that it is UNRECOVERABLE without
# healing, and that the fact should be visible in a diff if it changes.
#
#   DR-01  setup: the run actually floors coherence
#   DR-02  after flooring, quiet turns never return the level to NORMAL
#   DR-03  the calm counter never even reaches deescalate_sustained — the
#          de-escalation branch is not being entered, as distinct from being
#          entered and declining
#   DR-04  POSITIVE CONTROL — de-escalation is not broken. With the SAME
#          config, a run whose coherence never floors does step back down.
#          Without this, DR-02 and DR-03 pass for a build in which the level
#          simply never falls under any circumstances, which is the opposite
#          conclusion.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/deesc-reach-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT+1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_deescalation_reachability.sh"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
STUB_PID=""
cleanup() { [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null; teardown_isolated_trust; rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"

"$NAAB" --keygen "$TEST_TMP/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/k.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/k.pem"
export FAKE_KEY_DR="fake-key-deesc-reach"
sign_govern() { (cd "$1" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true; }
source "$SCRIPT_DIR/../helpers/stub_launch.sh"

# $1=outfile $2=floor|nofloor
gen_fixture() {
    python3 - "$1" "$2" <<'PYEOF'
import json, sys
mode = sys.argv[2]
r = [{"content": "Ledger reconcile step %d: quarterly totals computed and balance recorded." % i,
      "output_tokens": 45, "thinking_tokens": 20} for i in range(5)]
if mode == "floor":
    # Sustained unrelated drift: coherence collapses to 0 and stays there.
    sev = ["photography lenses","mountain weather","pasta recipes","jazz history","bicycle gears",
           "tide tables","volcano types","origami folds","desert beetles","harbour cranes"]
    for i, t in enumerate(sev):
        r.append({"content": "Consider %s." % t, "output_tokens": max(6, 18 - i), "thinking_tokens": 0})
    # Then quiet, on-instruction, mutually distinct. Nothing here should fire —
    # the point is that even total quiet does not bring the level down.
    for i in range(12):
        r.append({"content": "Resuming ledger reconcile: quarterly totals and balance verified for part %d." % i,
                  "output_tokens": 45, "thinking_tokens": 20})
else:
    # One brief off-topic turn: enough to cross elevated_threshold once, not
    # enough to floor coherence. Then straight back on task.
    r.append({"content": "Consider harbour cranes.", "output_tokens": 8, "thinking_tokens": 0})
    for i in range(16):
        r.append({"content": "Resuming ledger reconcile: quarterly totals and balance verified for part %d." % i,
                  "output_tokens": 45, "thinking_tokens": 20})
json.dump({"responses": r}, open(sys.argv[1], "w"))
PYEOF
}

# $1=name $2=floor|nofloor $3=sends $4=extra reality_checkpoint body (weights)
run_case() {
    WDIR="$TEST_TMP/$1"; mkdir -p "$WDIR"
    gen_fixture "$WDIR/fixture.json" "$2"
    start_stub "$WDIR/fixture.json" "$WDIR" || return 1
    cat > "$WDIR/govern.json" <<EOF
{
  "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl" },
  "behavioral_sequences": { "enabled": true },
  "context_drift": { "enabled": true, "level": "advisory", "check_interval_turns": 1,
    "adaptive_baseline_window": 5,
    "reality_checkpoint": { "enabled": false${4:-} } },
  "circuit_breaker": { "enabled": true,
    "elevated_threshold": 0.40, "elevated_sustained": 1,
    "high_threshold": 0.95, "critical_threshold": 0.99,
    "deescalate_sustained": 2,
    "level_effects": { "high_advisory_to_soft": false } },
  "agents": { "worker": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT",
      "api_key_env": "FAKE_KEY_DR", "max_tokens": 200, "max_turns": 60,
      "system_prompt": "You reconcile ledger data and report quarterly totals." } }
}
EOF
    sign_govern "$WDIR"
    cat > "$WDIR/t.naab" <<EOF
use agent
main {
    let h = agent.create("worker")
    let i = 0
    while i < $3 { let r = agent.send(h, "continue the ledger reconciliation") i = i + 1 }
    print("DONE")
}
EOF
    (cd "$WDIR" && timeout 250s "$NAAB" t.naab > out.txt 2> err.txt) || true
    stop_stub
    return 0
}

rows() {
    python3 - "$1" <<'PYEOF'
import json, sys
for line in open(sys.argv[1]):
    if '"CDD_TURN"' not in line: continue
    try: e = json.loads(line)
    except Exception: continue
    d = e.get("fields", e)
    if d.get("event_type") != "CDD_TURN" or d.get("analyzed") != "true": continue
    print("%s %s %s %s" % (d.get("turn"), d.get("coherence"),
                           d.get("governance_level"), d.get("deescalate_calm_turns", "0")))
PYEOF
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  De-escalation reachability once coherence floors            |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

echo -e "${CYAN}--- DR-01/02/03: floored coherence never comes back down ---${NC}"
if ! run_case floored floor 27; then
    skip "DR-01" "stub failed"
else
R=$(rows "$WDIR/tele.jsonl")
FLOOR_TURN=$(echo "$R" | awk '$2=="0.0000"{print $1; exit}')
if [ -n "$FLOOR_TURN" ]; then
    pass "DR-01" "coherence reached the floor at turn $FLOOR_TURN"
else
    fail "DR-01" "coherence never floored — DR-02/03 are not exercised" \
         "min coherence $(echo "$R" | awk '{print $2}' | sort -g | head -1)"
fi
if [ -n "$FLOOR_TURN" ]; then
    POST_NORMAL=$(echo "$R" | awk -v f="$FLOOR_TURN" '$1>f && $3=="normal"' | wc -l)
    POST_N=$(echo "$R" | awk -v f="$FLOOR_TURN" '$1>f' | wc -l)
    if [ "$POST_N" -ge 8 ] && [ "$POST_NORMAL" -eq 0 ]; then
        pass "DR-02" "level never returned to NORMAL across $POST_N post-floor turns"
    elif [ "$POST_N" -lt 8 ]; then
        fail "DR-02" "too few post-floor turns to be evidence" "only $POST_N"
    else
        fail "DR-02" "level returned to NORMAL after flooring — reachability changed" \
             "$POST_NORMAL of $POST_N post-floor turns at normal"
    fi
    MAXCALM=$(echo "$R" | awk -v f="$FLOOR_TURN" '$1>f{print $4}' | sort -n | tail -1)
    if [ "${MAXCALM:-0}" -lt 2 ]; then
        pass "DR-03" "calm counter never reached deescalate_sustained (max $MAXCALM of 2) — the branch is not entered"
    else
        fail "DR-03" "the de-escalation branch was entered but declined" \
             "max calm $MAXCALM — a different mechanism is holding the level"
    fi
fi
fi

echo -e "${CYAN}--- DR-04: POSITIVE CONTROL — neutralise the coherence term ---${NC}"
# THE SAME FIXTURE AND THE SAME DRIFT. The only change is the composite
# WEIGHT on coherence_proximity, set to 0 so pressure is signal density alone.
#
# This is the control that makes DR-02/03 mean something: de-escalation is NOT
# broken and the run is NOT too short. It is specifically the coherence term
# that holds the level up, and removing it brings the level down on identical
# input.
#
# TWO EARLIER VERSIONS OF THIS CONTROL WERE WRONG, both recorded because the
# wrong ones looked more natural than the right one:
#
#   "a run whose coherence never floors" — that arm did not de-escalate either.
#   With coherence_proximity carrying weight 0.35, signal_density 0.25 and
#   conversation_depth 0.10, the most that pressure can reach WITHOUT the
#   coherence term is 0.25 + 0.10 = 0.35, under elevated_threshold 0.40. So
#   escalation REQUIRES the coherence term to be live, which requires coherence
#   to have already fallen. Escalating and keeping coherence high are mutually
#   exclusive on a stock config.
#
#   "lower coherence_threshold to 0.05" — backwards. The threshold is the
#   DENOMINATOR of clamp(1 - coherence/threshold, 0, 1); lowering it makes the
#   term saturate SOONER, and at coherence 0 the term is 1.0 for any positive
#   threshold. Measured: coherence_prox=0.3500 at threshold 0.05, identical to
#   the default. test_deescalation_hysteresis.sh does not escape via the
#   threshold either — it zeroes the WEIGHT, which is what this arm does.
WEIGHTS=', "weights": { "coherence_proximity": 0.0, "signal_density": 1.0,
      "risk_score_proximity": 0.0, "conversation_depth": 0.0, "bsd_partial_progress": 0.0,
      "pipeline_inherited": 0.0, "coherence_acceleration": 0.0, "codegen_pressure": 0.0,
      "bsd_eviction_pressure": 0.0, "semantic_deviation": 0.0 }'
if ! run_case unfloored floor 27 "$WEIGHTS"; then
    skip "DR-04" "stub failed"
else
R2=$(rows "$WDIR/tele.jsonl")
ESCALATED=$(echo "$R2" | awk '$3!="normal"' | wc -l)
CAME_DOWN=$(echo "$R2" | awk 'prev!="" && prev!="normal" && $3=="normal"{c++} {prev=$3} END{print c+0}')
MINCOH2=$(echo "$R2" | awk '{print $2}' | sort -g | head -1)
if [ "$ESCALATED" -ge 1 ] && [ "$CAME_DOWN" -ge 1 ]; then
    pass "DR-04" "identical drift, coherence term neutralised: the level DOES step back down"
elif [ "$ESCALATED" -lt 1 ]; then
    fail "DR-04" "control never escalated — nothing to step down from" \
         "levels: $(echo "$R2" | awk '{print $3}' | sort -u | tr '\n' ' ')"
else
    fail "DR-04" "de-escalation never fires even with the coherence term off" \
         "min coherence $MINCOH2 — then something OTHER than coherence_prox holds the level, and DR-02/03 are misattributed"
fi
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
TOTAL=$((PASS_COUNT + FAIL_COUNT + SKIP_COUNT))
echo -e "  Total: $TOTAL | ${GREEN}Pass: $PASS_COUNT${NC} | ${RED}Fail: $FAIL_COUNT${NC} | ${YELLOW}Skip: $SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then echo -e "${RED}Failures:${NC}$FAILURES"; exit 1; fi
exit 0
