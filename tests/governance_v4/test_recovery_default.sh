#!/usr/bin/env bash
# ============================================================
# test_recovery_default.sh — ACCEPTANCE GATE, EXPECTED TO FAIL
#
# THIS FILE FAILS ON PURPOSE. It states what "an agent can recover" means under
# the DEFAULT configuration, and the engine does not do it yet. It is excluded
# from the governance_v4 sweep in run-all-tests.sh; deleting that line is the
# act of wiring it in, and belongs in the SAME commit that makes it pass — not
# before, or the suite stops being a signal.
#
#   Run it directly:  bash tests/governance_v4/test_recovery_default.sh
#
# THE CLAIM
#
# An agent that drifts, floors its coherence, and then does twenty-five turns of
# genuinely correct, on-instruction work should end the run recovered: coherence
# materially above the floor, and the governance level back at NORMAL. Nothing
# here sets coherence_natural_healing, deliberately — the whole point is what a
# stock configuration does.
#
# WHAT IT DOES TODAY (observed, this fixture, default config):
#
#     turn 16  high      0.0000     <- drift ends, recovery begins
#     turn 22  elevated  0.0000
#     turn 40  elevated  0.0000     <- 25 turns of correct work later
#
# Coherence does not move. The agent has no path back.
#
# WHY THIS IS A SEMANTICS MISMATCH, NOT A TUNING PREFERENCE
#
# Coherence is CONSUMED as a current-state signal — the output-admissibility
# gate asks "is this response coherent?" and coherence_proximity asks "how far
# below threshold are we now?", both present tense — and at defaults it is
# COMPUTED as worst-ever-sustained. There are three recovery channels, and the
# two that are live by default both require something external:
#
#     natural healing        coherence_natural_healing > 0   default 0
#     S22 validation credit  operator calls agent.record_validation()
#     challenge-pass credit  step_up_enabled                 default false
#
# So through its own behaviour alone, at defaults, an agent cannot recover.
#
# WHAT WOULD MAKE THIS PASS, AND THE CAUTION THAT GOES WITH IT
#
# Giving coherence_natural_healing a non-zero default. That errs toward
# LENIENCY — the direction that deserves the higher burden of proof — so it
# should not be done on the strength of this file alone. What makes it
# defensible is that healing is already bounded twice over:
# heal_factor = 1/(1 + signals_fired_this_turn) throttles it while signals are
# firing, and the damage ledger caps cumulative recovery at what was actually
# lost. Both must be verified live in the run that justifies the change, not
# merely cited — a disabled compensator and a broken component produce identical
# evidence.
#
# The companion test_coherence_recovery.sh pins the MECHANISM (healing controls
# recovery) with every arm's healing rate set explicitly, so it keeps passing
# whichever way the default goes. This file pins the DEFAULT. They are
# deliberately separate.
#
#   RD-01  setup: the drift phase floors coherence
#   RD-02  setup: the recovery phase is genuinely correct work (control)
#   RD-03  THE GATE — after 25 correct turns, coherence is off the floor
#   RD-04  THE GATE — and the governance level has returned to NORMAL
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/recdef-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT+1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_recovery_default.sh"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
STUB_PID=""
cleanup() { [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null; teardown_isolated_trust; rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"

"$NAAB" --keygen "$TEST_TMP/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/k.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/k.pem"
export FAKE_KEY_RD="fake-key-recovery-default"
source "$SCRIPT_DIR/../helpers/stub_launch.sh"

gen_fixture() {  # $1=outfile $2=full|recovery_only
    python3 - "$1" "$2" <<'PYEOF'
import json, sys
mode = sys.argv[2]
clean = "Ledger reconcile part %d: quarterly totals computed and the balance recorded."
verbs = ["recomputed","verified","reconciled","audited","closed"]
def recovery(n):
    return [{"content": "Ledger reconcile resumed: quarterly totals %s and the balance "
                        "recorded against source ledger section %d." % (verbs[i % 5], i),
             "output_tokens": 45, "thinking_tokens": 20} for i in range(n)]
if mode == "recovery_only":
    r = recovery(30)
else:
    r = [{"content": clean % i, "output_tokens": 45, "thinking_tokens": 20} for i in range(5)]
    drift = ["photography lenses","mountain weather","pasta recipes","jazz history","bicycle gears",
             "tide tables","volcano types","origami folds","desert beetles","harbour cranes"]
    for i, t in enumerate(drift):
        r.append({"content": "Consider %s." % t, "output_tokens": max(6, 18-i), "thinking_tokens": 0})
    r += recovery(25)
json.dump({"responses": r}, open(sys.argv[1], "w"))
PYEOF
}

# NOTE: coherence_natural_healing is NOT set anywhere below. That is the point.
run_case() {  # $1=name $2=mode $3=sends
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
    "adaptive_baseline_window": 5, "reality_checkpoint": { "enabled": false } },
  "circuit_breaker": { "enabled": true,
    "elevated_threshold": 0.40, "elevated_sustained": 1,
    "high_threshold": 0.60, "high_sustained": 2, "critical_threshold": 0.99,
    "deescalate_sustained": 2 },
  "agents": { "worker": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT",
      "api_key_env": "FAKE_KEY_RD", "max_tokens": 200, "max_turns": 80,
      "system_prompt": "You reconcile ledger data and report quarterly totals." } }
}
EOF
    (cd "$WDIR" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
    cat > "$WDIR/t.naab" <<EOF
use agent
main {
    let h = agent.create("worker")
    let i = 0
    while i < $3 { let r = agent.send(h, "continue the ledger reconciliation") i = i + 1 }
    print("DONE")
}
EOF
    (cd "$WDIR" && timeout 300s "$NAAB" t.naab > out.txt 2> err.txt) || true
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
    print("%s %s %s" % (d.get("turn"), d.get("governance_level"), d.get("coherence")))
PYEOF
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  ACCEPTANCE GATE — can an agent recover on a stock config?    |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

if ! run_case main full 40; then
    skip "RD-01" "stub failed to start"; exit 0
fi
R=$(rows "$WDIR/tele.jsonl")
MINC=$(echo "$R" | awk '{print $3}' | sort -g | head -1)
FINC=$(echo "$R" | tail -1 | awk '{print $3}')
FINL=$(echo "$R" | tail -1 | awk '{print $2}')

if [ -n "$MINC" ] && awk "BEGIN{exit !($MINC <= 0.0001)}"; then
    pass "RD-01" "setup: the drift phase floored coherence ($MINC)"
else
    fail "RD-01" "coherence never floored — the gate is not exercised" "min $MINC"
fi

if run_case control recovery_only 30; then
    RC_MIN=$(rows "$WDIR/tele.jsonl" | awk '{print $3}' | sort -g | head -1)
    if [ -n "$RC_MIN" ] && awk "BEGIN{exit !($RC_MIN >= 0.9999)}"; then
        pass "RD-02" "setup: the recovery turns are genuinely correct work (alone: $RC_MIN)"
    else
        fail "RD-02" "the 'recovery' turns are not clean — the gate would measure the fixture" \
             "min coherence alone: $RC_MIN"
    fi
else skip "RD-02" "stub failed"; fi

echo -e "${CYAN}--- THE GATE ---${NC}"
if [ -n "$FINC" ] && awk "BEGIN{exit !($FINC >= 0.30)}"; then
    pass "RD-03" "25 turns of correct work moved coherence off the floor ($FINC)"
else
    fail "RD-03" "25 turns of correct work earned nothing — coherence still $FINC" \
         "at defaults an agent has no path back through its own behaviour"
fi
if [ "$FINL" = "normal" ]; then
    pass "RD-04" "the governance level returned to NORMAL"
else
    fail "RD-04" "the agent is still governed at '$FINL' after recovering" \
         "scrutiny earned by drift is never released"
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
TOTAL=$((PASS_COUNT + FAIL_COUNT + SKIP_COUNT))
echo -e "  Total: $TOTAL | ${GREEN}Pass: $PASS_COUNT${NC} | ${RED}Fail: $FAIL_COUNT${NC} | ${YELLOW}Skip: $SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then echo -e "${RED}Failures:${NC}$FAILURES"; exit 1; fi
exit 0
