#!/usr/bin/env bash
# ============================================================
# test_deescalation_hysteresis.sh — governance level de-escalation hysteresis
#
# Escalation requires sustained pressure, but pre-fix a SINGLE calm
# composite sample recomputed the level from scratch and dropped it straight
# to NORMAL — scrutiny vanished exactly when a decaying agent briefly looked
# calm. Now stepping down requires circuit_breaker.deescalate_sustained
# consecutive calm turns and moves one level at a time.
#
# Deterministic pressure driver: reality_checkpoint weights put ALL weight on
# signal_density with divisor 1, so composite == 1.0 on turns where the only
# enabled signal (instruction_recall) fires and 0.0 on clean turns.
#
# Timeline (elevated_sustained=1, deescalate_sustained=2):
#   send 1: filler response  -> fires  -> composite 1.0 -> ELEVATED (fast up)
#   send 2: filler response  -> fires  -> stays ELEVATED
#   send 3: clean echo       -> calm 1 -> must STAY ELEVATED (pre-fix: normal)
#   send 4: clean echo       -> calm 2 -> steps down to NORMAL
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_deescalation_hysteresis.sh"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/deesc-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""

pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
STUB_PID=""
cleanup() {
    [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null
    teardown_isolated_trust
    rm -rf "$TEST_TMP"
}
trap cleanup EXIT
mkdir -p "$TEST_TMP"

"$NAAB" --keygen "$TEST_TMP/test-key.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/test-key.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/test-key.pem"
export FAKE_KEY_DEESC_TEST="fake-key-deescalation-test"

sign_govern() {
    (cd "$1" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
}

# Repointed at the shared hardened launcher (checksheet D1, "repoint on touch").
# The local copy replaced here picked a port with `(RANDOM % 20000) + 20000`
# once, with no bind check and no retry. On a loaded runner a collision or a
# lingering TIME_WAIT socket leaves the stub dead and every assertion in the
# suite then fails for a reason unrelated to what it measures -- passing locally
# and in a sibling CI job on the identical commit, which is what makes it cost a
# full diagnosis every time it fires.
#
# Two of these had already fired: test_config_adjustment.sh (CI red on a
# docs-only commit) and test_entity_window.sh (CI red while Build & Test passed
# on the same SHA). Fixing them one at a time as they fire is what this batch
# replaces; twelve suites shared this exact naive definition.
source "$SCRIPT_DIR/../helpers/stub_launch.sh"

cdd_level() {  # $1=telemetry file $2=nth CDD_TURN
    grep '"event_type":"CDD_TURN"' "$1" 2>/dev/null | sed -n "${2}p" \
        | grep -o '"governance_level":"[a-z]*"' | grep -o ':"[a-z]*"' | tr -d ':"'
}

cdd_field() {  # $1=telemetry file $2=nth CDD_TURN $3=field name
    grep '"event_type":"CDD_TURN"' "$1" 2>/dev/null | sed -n "${2}p" \
        | grep -o "\"$3\":\"[^\"]*\"" | head -1 | sed 's/.*":"//; s/"$//'
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|         Governance level de-escalation hysteresis            |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

WDIR="$TEST_TMP/de"; mkdir -p "$WDIR"
cat > "$WDIR/fixture.json" << 'EOF'
{"responses": [
  {"content": "reconcile the ledger: quarterly totals balance against the source", "output_tokens": 20},
  {"content": "meandering filler paragraph about nothing important whatsoever", "output_tokens": 20},
  {"content": "further meandering filler avoiding every requested subject entirely", "output_tokens": 20},
  {"content": "resuming ledger reconciliation quarterly totals verified balance", "output_tokens": 20},
  {"content": "ledger reconciliation continues quarterly totals remain balanced fine", "output_tokens": 20}
]}
EOF
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "D-00" "stub failed to start"; exit 1; }

cat > "$WDIR/govern.json" << GOVEOF
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "telemetry": { "enabled": true, "output_file": "telemetry.jsonl" },
    "behavioral_sequences": { "enabled": true },
    "context_drift": {
        "enabled": true, "level": "advisory", "check_interval_turns": 1,
        "coherence_threshold": 0.05,
        "adaptive_baseline_window": 1,
        "signals": {
            "circular_actions": false, "repeated_failures": false, "scope_creep": false,
            "intent_contradictions": false, "vocabulary_contraction": false,
            "coherence_velocity": false, "capability_underutilization": false,
            "response_quality": false, "thinking_collapse": false,
            "semantic_stability": false, "mandate_alignment": false,
            "context_growth": false, "instruction_recall": true, "plan_drift": false,
            "entity_consistency": false, "instruction_conflict": false,
            "persona_fingerprint": false, "tool_chain_integrity": false,
            "claim_result_reconciliation": false, "prompt_compliance": false,
            "response_repetition": false
        },
        "reality_checkpoint": {
            "enabled": false,
            "pressure_threshold": 0.5,
            "signal_density_divisor": 1,
            "weights": {
                "coherence_proximity": 0, "risk_score_proximity": 0,
                "signal_density": 1.0, "conversation_depth": 0,
                "bsd_partial_progress": 0, "pipeline_inherited": 0,
                "coherence_acceleration": 0, "codegen_pressure": 0,
                "bsd_eviction_pressure": 0, "semantic_deviation": 0
            }
        }
    },
    "circuit_breaker": {
        "enabled": true,
        "elevated_threshold": 0.5,
        "elevated_sustained": 1,
        "deescalate_sustained": 2
    },
    "agents": {
        "worker": {
            "provider": "gemini", "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": "FAKE_KEY_DEESC_TEST", "max_tokens": 100, "max_turns": 20
        }
    }
}
GOVEOF
sign_govern "$WDIR"

cat > "$WDIR/test.naab" << 'NAABEOF'
use agent
main {
    let h = agent.create("worker")
    // Turn 1 is a CLEAN calibration turn, not part of the scenario.
    //
    // adaptive_baseline_enabled defaults true. instruction_recall is the only
    // signal this suite enables and it is STATISTICAL, so inside the window it
    // is suppressed AND its firing rate is learned as normal. With the original
    // 4-send scenario every turn sat in the window: nothing escalated, and
    // D-02..D-07 all failed for want of a level to observe rather than because
    // hysteresis was broken.
    //
    // The window is pinned to 1 and given a clean turn to learn from, so the
    // learned rate is 0 and the first drift turn charges full weight. Turn
    // indices below are therefore the scenario turn PLUS ONE.
    let i = 0
    while i < 5 {
        let r = agent.send(h, "reconcile ledger quarterly totals balance")
        i = i + 1
    }
    print("DONE")
}
NAABEOF

OUTPUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1) || true
stop_stub

echo "$OUTPUT" | grep -q "DONE" && pass "D-01" "5 sends complete" \
    || fail "D-01" "sends did not complete" "$(echo "$OUTPUT" | head -3)"

# Scenario turn N is telemetry turn N+1 (turn 1 is the calibration warm-up).
L0=$(cdd_level "$WDIR/telemetry.jsonl" 1)
L1=$(cdd_level "$WDIR/telemetry.jsonl" 2)
L2=$(cdd_level "$WDIR/telemetry.jsonl" 3)
L3=$(cdd_level "$WDIR/telemetry.jsonl" 4)
L4=$(cdd_level "$WDIR/telemetry.jsonl" 5)

# The warm-up must NOT have escalated, or the scenario starts from the wrong
# state and D-02 passes for a reason that has nothing to do with drift.
if [ "$L0" = "normal" ]; then
    pass "D-01b" "Calibration warm-up stayed NORMAL (clean baseline)"
else
    fail "D-01b" "Warm-up escalated — baseline is contaminated" "turn1 level=$L0"
fi

if [ "$L1" = "elevated" ]; then
    pass "D-02" "Escalation is immediate on the first high-pressure turn"
else
    fail "D-02" "Fast escalation broken" "turn1 level=$L1"
fi
if [ "$L2" = "elevated" ]; then
    pass "D-03" "Sustained pressure holds ELEVATED"
else
    fail "D-03" "Level lost under sustained pressure" "turn2 level=$L2"
fi
if [ "$L3" = "elevated" ]; then
    pass "D-04" "One calm turn does NOT de-escalate (hysteresis)"
else
    fail "D-04" "Single calm sample de-escalated (pre-fix behavior)" "turn3 level=$L3"
fi
if [ "$L4" = "normal" ]; then
    pass "D-05" "Second consecutive calm turn steps down to NORMAL"
else
    fail "D-05" "De-escalation after sustained calm broken" "turn4 level=$L4"
fi

# D-06: the counter must report the value that FIRED the step-down.
#
# deescalate_calm_turns_ is live state and is reset the instant the step-down
# fires; the CDD_TURN row is written afterwards. So the field originally read 0
# on exactly the turn the value mattered, and its visible maximum was
# deescalate_sustained - 1 on any run that stepped down — which made
# max(field) look comparable to deescalate_sustained while being reliably off by
# one. Telemetry now reports the value the engine EVALUATED this turn.
#
# This assertion FAILS if that fix is reverted: pre-fix the firing turn reports
# 0. deescalate_sustained is 2 in this config and turn 4 is the step-down (D-05).
CALM4=$(cdd_field "$WDIR/telemetry.jsonl" 5 "deescalate_calm_turns")
if [ "$CALM4" = "2" ]; then
    pass "D-06" "Firing turn reports the count that fired it (calm=2 = deescalate_sustained)"
elif [ "$CALM4" = "0" ]; then
    fail "D-06" "Firing turn reports 0 — the counter was reset before the row was written" \
         "pre-fix behaviour: max(field) understates deescalate_sustained by one"
else
    fail "D-06" "Firing turn reports an unexpected calm count" "got '$CALM4', expected 2"
fi

# D-07 is the control for D-06: a NON-firing calm turn must still report its
# running count, so D-06 cannot pass by the field being hardcoded to the
# threshold. Turn 3 is the first calm turn (D-04 pins that it did NOT step down).
CALM3=$(cdd_field "$WDIR/telemetry.jsonl" 4 "deescalate_calm_turns")
if [ "$CALM3" = "1" ]; then
    pass "D-07" "Non-firing calm turn reports its running count (calm=1) (control)"
else
    fail "D-07" "Non-firing calm turn reports '$CALM3', expected 1" \
         "without this, D-06 would pass on a field pinned to deescalate_sustained"
fi

echo ""
echo "================================================"
echo "  PASS: $PASS_COUNT  FAIL: $FAIL_COUNT  SKIP: $SKIP_COUNT"
echo "================================================"
[ -n "$FAILURES" ] && echo -e "Failures:$FAILURES"

exit "$FAIL_COUNT"
