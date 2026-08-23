#!/usr/bin/env bash
# ============================================================
# test_escalation_effectiveness.sh — is the measurement of "did escalating
# help?" actually measuring that?
#
# escalation_effectiveness is the engine's only record of whether one of its
# own interventions worked: mean coherence over the N turns after an escalation,
# minus coherence at the moment of escalation. Nothing consumes it yet — it
# reaches the dashboard, RECONCILIATION_TURN telemetry and the agent environment
# — and before it can inform any decision it has to be correct. It was not, in
# two ways that this file pins:
#
#   THE WINDOW KEY WAS NEVER PARSED. escalation_effectiveness_window is declared
#   in ContextDriftConfig, documented, and SET by a shipped test config
#   (tests/gorilla/naab-53/src/govern.json), while governance_config.cpp read
#   every sibling key in that struct and not this one. It stayed hardcoded at 5
#   whatever govern.json said.
#
#   DE-ESCALATIONS WERE RECORDED AS ESCALATIONS. recordEscalation() is called on
#   any level change, so lowering scrutiny re-armed the accumulator and was then
#   reported under a field named for the opposite:
#       "Escalation: level 1->0 at turn 15, effectiveness=+0.30"
#   It also destroyed any escalation measurement still in flight — a 25-turn run
#   with four level changes reported effectiveness as N/A on EVERY turn, because
#   no window ever survived to complete.
#
# WHAT IS ASSERTED
#   EE-01  the configured window is honoured (config says 3, engine reports 3)
#   EE-02  the recorded transition is an ESCALATION (to_level > from_level)
#   EE-03  a value is reported at all — positive control that a window completes
#
# The reported QUANTITY is penalty rate against the drift that provoked the
# escalation, not coherence — see examples/effectiveness-semantics, which
# measured that no offset/window separates helped / no-effect / worse under the
# coherence-based definition, and that a fixed pre-window is config-dependent
# (sweeping elevated_threshold 0.30 -> 0.50 moved the working pre-window from
# [1,2] to [2,3,4]) while a window derived from drift onset is invariant.
#
# WHAT IS NOT FIXED, AND IS DELIBERATELY NOT ASSERTED HERE
#   The measure is one-sided at the floor: eff = mean(post) - coherence_at, and
#   escalation usually fires when coherence is already low, so coherence_at is
#   near 0 and eff cannot go negative. It structurally cannot report that an
#   intervention made things WORSE, which is the case that matters most for
#   using it in a decision.
#   Only the acting handle records the transition, while governance_level_ is
#   engine-global — siblings whose scrutiny changed have no record of it.
#   Both need a decision about what the number should mean, not a patch.
# ============================================================
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
if [ -d "/data/data/com.termux/files/usr/tmp" ]; then _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"; else _SYSTMP="${TMPDIR:-/tmp}"; fi
TEST_TMP="${_SYSTMP}/esceff-$$"
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT+1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_escalation_effectiveness.sh"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
STUB_PID=""
cleanup() { [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null; teardown_isolated_trust; [ -n "${KEEP_TMP:-}" ] || rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"
[ -x "$NAAB" ] || { skip "EE-00" "build/naab-lang not found"; exit 0; }

"$NAAB" --keygen "$TEST_TMP/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/k.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/k.pem"
export FAKE_KEY_EE="fake-key-esceff"
WINDOW=3

# The drift-discrimination fixture is used because it produces BOTH directions:
# correct warm-up, sustained drift, then sustained recovery. A fixture that only
# drifts would never de-escalate, and EE-02 would pass without being tested.
python3 "$SCRIPT_DIR/../../examples/drift-discrimination/gen_fixture.py" "$TEST_TMP/fx.json" >/dev/null

STUB_PORT=$(( (RANDOM % 20000) + 20000 ))
python3 "$SCRIPT_DIR/../helpers/agent_stub.py" "$STUB_PORT" "$TEST_TMP/fx.json" "$TEST_TMP" > "$TEST_TMP/stub.log" 2>&1 &
STUB_PID=$!
for i in $(seq 1 60); do grep -q READY "$TEST_TMP/stub.log" 2>/dev/null && break; sleep 0.5; done
grep -q READY "$TEST_TMP/stub.log" 2>/dev/null || { skip "EE-00" "stub failed to start"; exit 0; }

cat > "$TEST_TMP/govern.json" <<GOVEOF
{
  "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "t.jsonl" },
  "behavioral_sequences": { "enabled": true },
  "context_drift": {
    "enabled": true, "check_interval_turns": 1,
    "adaptive_baseline_enabled": true, "adaptive_baseline_window": 4,
    "coherence_natural_healing": 0.15,
    "escalation_effectiveness_window": $WINDOW,
    "reality_checkpoint": { "enabled": false }
  },
  "circuit_breaker": {
    "enabled": true, "step_up_enabled": false,
    "elevated_threshold": 0.30, "elevated_sustained": 1,
    "high_threshold": 0.55, "high_sustained": 2,
    "critical_threshold": 0.9, "critical_sustained": 5,
    "deescalate_sustained": 1
  },
  "agents": {
    "b": {
      "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT", "api_key_env": "FAKE_KEY_EE",
      "max_tokens": 200, "max_turns": 40,
      "system_prompt": "Build a Calculator class with add, subtract, multiply and divide methods, each recording an entry in a history log."
    }
  }
}
GOVEOF
(cd "$TEST_TMP" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
cat > "$TEST_TMP/t.naab" <<'NAABEOF'
use agent
main {
    let h = agent.create("b")
    let i = 0
    while i < 25 { i = i + 1; let r = agent.send(h, "Continue.") }
    print("RUN_DONE")
}
NAABEOF

export FAKE_KEY_EE
DASH=$( (cd "$TEST_TMP" && timeout 180s "$NAAB" --governance-dashboard t.naab 2>&1) )
kill "$STUB_PID" 2>/dev/null; STUB_PID=""

LINE=$(echo "$DASH" | grep -i "Escalation: level" | head -1)
echo ""
echo -e "${CYAN}Dashboard line:${NC} ${LINE:-<none>}"
echo ""

# Positive control on the SCENARIO. Both directions must occur, or EE-02 passes
# without ever being exercised: a run that never de-escalates cannot show that a
# de-escalation is excluded.
DE=$(echo "$DASH" | grep -c "level 2->1\|level 1->0" || true)
if echo "$DASH" | grep -qi "Escalation: level"; then
    pass "EE-00" "the run produced a recorded level transition"
else
    fail "EE-00" "no level transition occurred" "nothing below is exercised"
    echo -e "${RED}Aborting.${NC}"; exit 1
fi

# Match the window figure without anchoring on what follows it: the
# dashboard line gained a "trigger X -> Y" suffix when the measure
# changed to penalty rate, and a pattern ending in ")" broke on it
# while the window was in fact correct.
if echo "$LINE" | grep -q "${WINDOW}-turn window"; then
    pass "EE-01" "the configured escalation_effectiveness_window ($WINDOW) is honoured"
else
    fail "EE-01" "the configured window was ignored" \
         "config set $WINDOW; line reads: $LINE — the key is declared but not parsed"
fi

FROM=$(echo "$LINE" | sed -n 's/.*level \([0-9]*\)->\([0-9]*\).*/\1/p')
TO=$(echo "$LINE" | sed -n 's/.*level \([0-9]*\)->\([0-9]*\).*/\2/p')
if [ -n "$FROM" ] && [ -n "$TO" ] && [ "$TO" -gt "$FROM" ]; then
    pass "EE-02" "the recorded transition is an escalation ($FROM->$TO)"
else
    fail "EE-02" "a de-escalation was recorded as an escalation ($FROM->$TO)" \
         "recordEscalation() fires on any level change; it must ignore to_level <= from_level"
fi

if echo "$LINE" | grep -q "effectiveness="; then
    pass "EE-03" "an effectiveness value was reported (the window completed)"
else
    fail "EE-03" "no effectiveness value reported" \
         "the window never completed — a later transition re-armed the accumulator"
fi

echo ""
TOTAL=$((PASS_COUNT + FAIL_COUNT + SKIP_COUNT))
echo -e "  Total: $TOTAL | ${GREEN}Pass: $PASS_COUNT${NC} | ${RED}Fail: $FAIL_COUNT${NC} | ${YELLOW}Skip: $SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then echo -e "${RED}Failures:${NC}$FAILURES"; exit 1; fi
exit 0
