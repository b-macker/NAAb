#!/usr/bin/env bash
# ============================================================
# test_challenge_fail_path.sh — step-up challenge FAILURE path
#
# Across five live living-script runs the challenge system produced exactly
# one failure — the fail path had no deterministic coverage (gorilla tests
# only count failures a live model happens to produce). This test forces
# both outcomes with the loopback stub:
#
# Group A: an off-topic challenge answer FAILS the challenge — the engine
#          throws GovernanceHardError ("Step-up challenge failed"), the
#          process exits 3, and AGENT_CHALLENGE_FAIL telemetry carries the
#          challenge_type and keyword_ratio. This is the same governance-
#          kill class the living-script harnesses classify as
#          "challenge_failure".
# Group B: control — an on-topic answer PASSES the same challenge and the
#          gated send completes normally (exit 0).
#
# Escalation staging mirrors test_challenge_first_turn.sh: S22 fires once
# (validation failure), signal_density alone drives the level to ELEVATED,
# where step-up gates the next send. The recorded failure detail makes the
# challenge the priority-0 "validation" type.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/challenge-fail-$$"

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
export FAKE_KEY_CHALFAIL="fake-key-challenge-fail"

sign_govern() { (cd "$1" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true; }

start_stub() {  # $1=fixture $2=workdir
    STUB_PORT=$(( (RANDOM % 20000) + 20000 ))
    python3 "$SCRIPT_DIR/../helpers/agent_stub.py" "$STUB_PORT" "$1" "$2" > "$2/stub.log" 2>&1 &
    STUB_PID=$!
    for _ in $(seq 1 50); do grep -q READY "$2/stub.log" 2>/dev/null && return 0; sleep 0.1; done
    return 1
}
stop_stub() { [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null; wait "$STUB_PID" 2>/dev/null; STUB_PID=""; }

mk_govern() {  # $1=port
    cat <<EOF
{
  "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl" },
  "behavioral_sequences": { "enabled": true },
  "context_drift": {
    "enabled": true, "level": "advisory", "check_interval_turns": 1,
    "signals": {
      "circular_actions": false, "repeated_failures": false, "scope_creep": false,
      "intent_contradictions": false, "coherence_velocity": false,
      "response_quality": false, "thinking_collapse": false,
      "semantic_stability": false, "mandate_alignment": false,
      "context_growth": false, "instruction_recall": false, "plan_drift": false,
      "entity_consistency": false, "instruction_conflict": false,
      "persona_fingerprint": false, "tool_chain_integrity": false,
      "claim_result_reconciliation": false, "prompt_compliance": false,
      "response_repetition": false, "validation_outcome": true
    },
    "reality_checkpoint": {
      "enabled": false, "pressure_threshold": 0.5, "signal_density_divisor": 1,
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
    "enabled": true, "elevated_threshold": 0.5, "elevated_sustained": 1,
    "deescalate_sustained": 5,
    "step_up_enabled": true, "step_up_contextual": true,
    "step_up_at_level": "elevated"
  },
  "agents": { "developer": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$1",
      "api_key_env": "FAKE_KEY_CHALFAIL", "max_tokens": 100, "max_turns": 20 } }
}
EOF
}

# Test script shared by both groups: send1 delivers, a failing validation with
# detail escalates via S22, send2 draws the (validation-type) challenge whose
# answer is the stub's next fixture response, send3 only runs if send2 survived.
TEST_NAAB='use agent
main {
    let h = agent.create("developer")
    let r1 = agent.send(h, "implement divide")
    let _f = agent.record_validation(h, false, "FAILED test_divide_by_zero: divide raised ZeroDivisionError, expected ValueError for zero divisor")
    let r2 = agent.send(h, "fix the divide operation")
    print("SEND2_OK")
    let r3 = agent.send(h, "add divide docstring")
    print("SEND3_OK")
}'

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|   Step-up challenge FAILURE path (deterministic, stub)       |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

# ============================================================
# Group A: off-topic challenge answer → FAIL → GovernanceHardError, exit 3
# ============================================================
echo -e "${CYAN}--- Group A: failed challenge kills the run (exit 3) ---${NC}"
WDIR="$TEST_TMP/a"; mkdir -p "$WDIR"
# Response order: r1=send1, r2=send2 (escalation lands in its post-CDD),
# r3=the CHALLENGE answer gating send3 — maximally off-topic so keyword
# overlap with the validation-failure detail is ~zero, r4=unreached.
printf '%s' '{"responses": [
  {"content": "def divide(a, b): return a / b  # implemented divide operation", "output_tokens": 30},
  {"content": "def divide(a, b): return a / b  # attempting fix", "output_tokens": 30},
  {"content": "banana umbrella weather picnic sunshine holiday melody", "output_tokens": 30},
  {"content": "def divide(a, b): return a / b  # docstring added", "output_tokens": 30}
]}' > "$WDIR/fixture.json"
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "A-00" "stub failed"; STUB_PORT=0; }
if [ "$STUB_PORT" != "0" ]; then
mk_govern "$STUB_PORT" > "$WDIR/govern.json"; sign_govern "$WDIR"
printf '%s' "$TEST_NAAB" > "$WDIR/test.naab"
OUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1); EXIT_A=$?
stop_stub
if [ "$EXIT_A" -eq 3 ]; then
    pass "A-01" "Failed challenge exits 3 (GovernanceHardError)"
else
    fail "A-01" "Wrong exit code" "exit=$EXIT_A, expected 3"
fi
if echo "$OUT" | grep -q "Step-up challenge failed"; then
    pass "A-02" "Kill message surfaced on stdout (harness-detectable signature)"
else
    fail "A-02" "No 'Step-up challenge failed' in output" "$(echo "$OUT" | tail -3)"
fi
CHAL_A=$(grep '"event_type":"AGENT_CHALLENGE_FAIL"' "$WDIR/tele.jsonl" 2>/dev/null | head -1)
if [ -n "$CHAL_A" ]; then
    pass "A-03" "AGENT_CHALLENGE_FAIL telemetry emitted before the throw"
else
    fail "A-03" "No AGENT_CHALLENGE_FAIL event" "$(grep -c CDD_TURN "$WDIR/tele.jsonl" 2>/dev/null) CDD turns"
fi
if echo "$CHAL_A" | grep -q '"challenge_type":"validation"'; then
    pass "A-04" "Failed challenge was the validation (competence) type"
else
    fail "A-04" "Unexpected challenge type" "$CHAL_A"
fi
RATIO_A=$(echo "$CHAL_A" | grep -oP '"keyword_ratio":"\K[0-9.]+' | head -1)
if [ -n "$RATIO_A" ] && awk "BEGIN{exit !($RATIO_A < 0.30)}"; then
    pass "A-05" "keyword_ratio below contextual threshold ($RATIO_A < 0.30)"
else
    fail "A-05" "keyword_ratio not below threshold" "ratio=$RATIO_A"
fi
# Escalation lands during send2's post-response CDD, so send2 completes and
# the CHALLENGE gates send3 — which must never finish after the failed gate.
if echo "$OUT" | grep -q "SEND2_OK" && ! echo "$OUT" | grep -q "SEND3_OK"; then
    pass "A-06" "Gated send (send3) never completed; pre-gate send2 did"
else
    fail "A-06" "Unexpected send completion pattern" "$(echo "$OUT" | grep -c SEND2_OK)x SEND2, $(echo "$OUT" | grep -c SEND3_OK)x SEND3"
fi
if ! grep -q '"event_type":"AGENT_CHALLENGE_PASS"' "$WDIR/tele.jsonl" 2>/dev/null; then
    pass "A-07" "No spurious CHALLENGE_PASS (no recovery credited on fail)"
else
    fail "A-07" "CHALLENGE_PASS present in a fail-only scenario"
fi
fi

# ============================================================
# Group B: control — on-topic answer passes the same challenge
# ============================================================
echo -e "${CYAN}--- Group B: on-topic answer passes the same gate ---${NC}"
WDIR="$TEST_TMP/b"; mkdir -p "$WDIR"
# r3 is the CHALLENGE answer: on-topic, restating the recorded defect.
printf '%s' '{"responses": [
  {"content": "def divide(a, b): return a / b  # implemented divide operation", "output_tokens": 30},
  {"content": "def divide(a, b): return a / b  # attempting fix", "output_tokens": 30},
  {"content": "The divide test failed because divide raised ZeroDivisionError instead of ValueError for a zero divisor. I will catch ZeroDivisionError and raise ValueError, then proceed.", "output_tokens": 40},
  {"content": "def divide(a, b): return a / b  # docstring added", "output_tokens": 30}
]}' > "$WDIR/fixture.json"
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "B-00" "stub failed"; STUB_PORT=0; }
if [ "$STUB_PORT" != "0" ]; then
mk_govern "$STUB_PORT" > "$WDIR/govern.json"; sign_govern "$WDIR"
printf '%s' "$TEST_NAAB" > "$WDIR/test.naab"
OUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1); EXIT_B=$?
stop_stub
if [ "$EXIT_B" -eq 0 ] && echo "$OUT" | grep -q "SEND3_OK"; then
    pass "B-01" "On-topic answer passes; script completes (exit 0)"
else
    fail "B-01" "Control run did not complete" "exit=$EXIT_B $(echo "$OUT" | tail -2)"
fi
CHAL_B=$(grep '"event_type":"AGENT_CHALLENGE_PASS"' "$WDIR/tele.jsonl" 2>/dev/null | head -1)
if echo "$CHAL_B" | grep -q '"challenge_type":"validation"'; then
    pass "B-02" "Same validation-type challenge, passed with on-topic answer"
else
    fail "B-02" "No validation-type CHALLENGE_PASS" "$CHAL_B"
fi
fi

# ============================================================
echo ""
echo -e "${CYAN}+==============================================================+${NC}"
TOTAL=$((PASS_COUNT + FAIL_COUNT + SKIP_COUNT))
echo -e "  Total: $TOTAL | ${GREEN}Pass: $PASS_COUNT${NC} | ${RED}Fail: $FAIL_COUNT${NC} | ${YELLOW}Skip: $SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then echo -e "${RED}Failures:${NC}$FAILURES"; exit 1; fi
exit 0
