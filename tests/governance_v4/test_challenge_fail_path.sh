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
# Group C: max_challenge_failures=2 — first fail blocks the send with a
#          CATCHABLE error; a later passed challenge resets the streak and
#          the run completes.
# Group D: max_challenge_failures=2 — two consecutive fails terminate (exit 3).
# Group E: raising max_challenge_failures mid-run = ratchet violation.
# Group F: step_up_on_inadmissible — sub-OA coherence draws a challenge with
#          the engine-global level still NORMAL (pressure weights zeroed);
#          control run without the flag gets no challenge.
# Group G: disabling step_up_on_inadmissible mid-run = ratchet violation.
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

mk_govern() {  # $1=port $2=extra circuit_breaker fields (e.g. ', "max_challenge_failures": 2')
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
    "step_up_at_level": "elevated"${2:-}
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
mk_govern "$STUB_PORT" "" > "$WDIR/govern.json"; sign_govern "$WDIR"
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
mk_govern "$STUB_PORT" "" > "$WDIR/govern.json"; sign_govern "$WDIR"
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
# Group C: streak=2 — first fail is catchable, pass resets, run completes
# ============================================================
echo -e "${CYAN}--- Group C: first fail survivable under max_challenge_failures=2 ---${NC}"
WDIR="$TEST_TMP/c"; mkdir -p "$WDIR"
# API call order: r1=send1, r2=send2, r3=challenge#1 (off-topic -> FAIL,
# catchable), r4=challenge#2 (on-topic -> PASS, resets streak), r5=send4.
printf '%s' '{"responses": [
  {"content": "def divide(a, b): return a / b  # implemented divide operation", "output_tokens": 30},
  {"content": "def divide(a, b): return a / b  # attempting fix", "output_tokens": 30},
  {"content": "banana umbrella weather picnic sunshine holiday melody", "output_tokens": 30},
  {"content": "The divide test failed because divide raised ZeroDivisionError instead of ValueError for a zero divisor. I will catch ZeroDivisionError and raise ValueError, then proceed.", "output_tokens": 40},
  {"content": "def divide(a, b): return a / b  # docstring added", "output_tokens": 30}
]}' > "$WDIR/fixture.json"
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "C-00" "stub failed"; STUB_PORT=0; }
if [ "$STUB_PORT" != "0" ]; then
mk_govern "$STUB_PORT" ', "step_up_cooldown_turns": 0, "max_challenge_failures": 2' > "$WDIR/govern.json"; sign_govern "$WDIR"
cat > "$WDIR/test.naab" <<'EOF'
use agent
main {
    let h = agent.create("developer")
    let r1 = agent.send(h, "implement divide")
    let _f = agent.record_validation(h, false, "FAILED test_divide_by_zero: divide raised ZeroDivisionError, expected ValueError for zero divisor")
    let r2 = agent.send(h, "fix the divide operation")
    try {
        let r3 = agent.send(h, "add divide docstring")
        print("SEND3_OK")
    } catch (e) {
        print("SEND3_BLOCKED")
    }
    let r4 = agent.send(h, "add divide docstring again")
    print("SEND4_OK")
}
EOF
OUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1); EXIT_C=$?
stop_stub
if [ "$EXIT_C" -eq 0 ] && echo "$OUT" | grep -q "SEND3_BLOCKED" && echo "$OUT" | grep -q "SEND4_OK"; then
    pass "C-01" "First fail blocked catchably; run continued and completed"
else
    fail "C-01" "Streak-tolerant path broken" "exit=$EXIT_C blocked=$(echo "$OUT" | grep -c SEND3_BLOCKED) s4=$(echo "$OUT" | grep -c SEND4_OK)"
fi
if grep '"event_type":"AGENT_CHALLENGE_FAIL"' "$WDIR/tele.jsonl" 2>/dev/null | grep -q '"failure_streak":"1"'; then
    pass "C-02" "CHALLENGE_FAIL telemetry carries failure_streak=1"
else
    fail "C-02" "failure_streak missing/wrong" "$(grep 'CHALLENGE_FAIL' "$WDIR/tele.jsonl" | head -1)"
fi
if grep '"event_type":"AGENT_CHALLENGE_FAIL"' "$WDIR/tele.jsonl" 2>/dev/null | grep -q '"max_allowed":"2"'; then
    pass "C-03" "CHALLENGE_FAIL telemetry carries max_allowed=2"
else
    fail "C-03" "max_allowed missing/wrong"
fi
if grep -q '"event_type":"AGENT_CHALLENGE_PASS"' "$WDIR/tele.jsonl" 2>/dev/null; then
    pass "C-04" "Re-challenge after the blocked send was passable (streak reset)"
else
    fail "C-04" "No CHALLENGE_PASS after recovery"
fi
fi

# ============================================================
# Group D: streak=2 — consecutive fails terminate (exit 3)
# ============================================================
echo -e "${CYAN}--- Group D: consecutive fails still kill ---${NC}"
WDIR="$TEST_TMP/d"; mkdir -p "$WDIR"
printf '%s' '{"responses": [
  {"content": "def divide(a, b): return a / b  # implemented divide operation", "output_tokens": 30},
  {"content": "def divide(a, b): return a / b  # attempting fix", "output_tokens": 30},
  {"content": "banana umbrella weather picnic sunshine holiday melody", "output_tokens": 30},
  {"content": "cloud raindrop kettle sofa lantern gravel acorn", "output_tokens": 30},
  {"content": "def divide(a, b): return a / b  # docstring added", "output_tokens": 30}
]}' > "$WDIR/fixture.json"
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "D-00" "stub failed"; STUB_PORT=0; }
if [ "$STUB_PORT" != "0" ]; then
mk_govern "$STUB_PORT" ', "step_up_cooldown_turns": 0, "max_challenge_failures": 2' > "$WDIR/govern.json"; sign_govern "$WDIR"
cat > "$WDIR/test.naab" <<'EOF'
use agent
main {
    let h = agent.create("developer")
    let r1 = agent.send(h, "implement divide")
    let _f = agent.record_validation(h, false, "FAILED test_divide_by_zero: divide raised ZeroDivisionError, expected ValueError for zero divisor")
    let r2 = agent.send(h, "fix the divide operation")
    try {
        let r3 = agent.send(h, "add divide docstring")
        print("SEND3_OK")
    } catch (e) {
        print("SEND3_BLOCKED")
    }
    let r4 = agent.send(h, "add divide docstring again")
    print("SEND4_OK")
}
EOF
OUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1); EXIT_D=$?
stop_stub
if [ "$EXIT_D" -eq 3 ] && ! echo "$OUT" | grep -q "SEND4_OK"; then
    pass "D-01" "Second consecutive fail terminates (exit 3, NAAb catch cannot swallow)"
else
    fail "D-01" "Streak limit not enforced" "exit=$EXIT_D s4=$(echo "$OUT" | grep -c SEND4_OK)"
fi
if grep '"event_type":"AGENT_CHALLENGE_FAIL"' "$WDIR/tele.jsonl" 2>/dev/null | grep -q '"failure_streak":"2"'; then
    pass "D-02" "Second CHALLENGE_FAIL carries failure_streak=2"
else
    fail "D-02" "failure_streak=2 not recorded" "$(grep -c CHALLENGE_FAIL "$WDIR/tele.jsonl") fail events"
fi
fi

# ============================================================
# Group E: raising max_challenge_failures mid-run = ratchet violation
# ============================================================
echo -e "${CYAN}--- Group E: raising the streak limit is a ratchet violation ---${NC}"
IS_WINDOWS=false
if [[ "$(uname -s)" == MINGW* ]] || [[ "$(uname -s)" == MSYS* ]] || [[ -n "${WINDIR:-}" ]]; then IS_WINDOWS=true; fi
if $IS_WINDOWS; then
    skip "E-01" "mid-run file swap requires POSIX file semantics"
else
WDIR="$TEST_TMP/e"; mkdir -p "$WDIR"
printf '%s' '{"responses": [
  {"content": "def add(a, b): return a + b  # implemented add", "output_tokens": 30},
  {"content": "def subtract(a, b): return a - b  # implemented subtract", "output_tokens": 30}
]}' > "$WDIR/fixture.json"
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "E-00" "stub failed"; STUB_PORT=0; }
if [ "$STUB_PORT" != "0" ]; then
presign() {
    local pdir="$TEST_TMP/.presign"; mkdir -p "$pdir"
    printf '%s' "$1" > "$pdir/govern.json"; sign_govern "$pdir"
    cat "$pdir/govern.json.sig" 2>/dev/null || echo ""; rm -rf "$pdir"
}
LOOSE_E="{\"version\":\"5.0\",\"mode\":\"enforce\",\"security\":{\"sandbox_level\":\"elevated\"},\"telemetry\":{\"enabled\":true,\"output_file\":\"tele.jsonl\"},\"behavioral_sequences\":{\"enabled\":true},\"context_drift\":{\"enabled\":true,\"level\":\"advisory\",\"check_interval_turns\":1},\"circuit_breaker\":{\"enabled\":true,\"step_up_enabled\":true,\"max_challenge_failures\":5},\"capabilities\":{\"shell\":{\"enabled\":true}},\"agents\":{\"developer\":{\"provider\":\"gemini\",\"model\":\"stub-model\",\"api_base\":\"http://127.0.0.1:$STUB_PORT\",\"api_key_env\":\"FAKE_KEY_CHALFAIL\",\"max_tokens\":100,\"max_turns\":20}}}"
LOOSE_E_SIG=$(presign "$LOOSE_E")
if [ -z "$LOOSE_E_SIG" ]; then skip "E-01" "presign failed"; stop_stub; else
export NAAB_LOOSE_E_JSON="$LOOSE_E" NAAB_LOOSE_E_SIG="$LOOSE_E_SIG"
cat > "$WDIR/govern.json" <<EOF
{
  "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl" },
  "behavioral_sequences": { "enabled": true },
  "context_drift": { "enabled": true, "level": "advisory", "check_interval_turns": 1 },
  "circuit_breaker": { "enabled": true, "step_up_enabled": true, "max_challenge_failures": 2 },
  "capabilities": { "shell": { "enabled": true } },
  "agents": { "developer": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT",
      "api_key_env": "FAKE_KEY_CHALFAIL", "max_tokens": 100, "max_turns": 20 } }
}
EOF
sign_govern "$WDIR"
cat > "$WDIR/test.naab" <<'EOF'
use agent
main {
    let h = agent.create("developer")
    let r1 = agent.send(h, "implement add")
    let _ = <<shell
sleep 1.2
printf '%s' "$NAAB_LOOSE_E_JSON" > govern.json
printf '%s' "$NAAB_LOOSE_E_SIG" > govern.json.sig
>>
    let r2 = agent.send(h, "implement subtract")
    print("DONE")
}
EOF
OUT=$(cd "$WDIR" && timeout 40s "$NAAB" test.naab 2>"$WDIR/stderr.txt") || true
stop_stub
if grep -q "ratchet violation" "$WDIR/stderr.txt" 2>/dev/null && grep -q "max_challenge_failures" "$WDIR/stderr.txt" 2>/dev/null; then
    pass "E-01" "mid-run raise of max_challenge_failures rejected as ratchet violation"
else
    fail "E-01" "mid-run raise not rejected" "$(grep -i 'ratchet\|reload' "$WDIR/stderr.txt" 2>/dev/null | head -2)"
fi
fi
fi
fi

# ============================================================
# Group F: coherence-floor trigger — recovery ladder in the sub-OA dead zone
# ============================================================
echo -e "${CYAN}--- Group F: sub-OA coherence draws a challenge at NORMAL level ---${NC}"
# Pressure weights all zero => engine-global level stays NORMAL, so the ONLY
# way a challenge can fire is the coherence-floor trigger. Three consumed S22
# failures put coherence at 0.55 (< OA threshold 0.60) by send 4.
mk_govern_floor() {  # $1=port $2=step_up_on_inadmissible(true/false)
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
        "signal_density": 0, "conversation_depth": 0,
        "bsd_partial_progress": 0, "pipeline_inherited": 0,
        "coherence_acceleration": 0, "codegen_pressure": 0,
        "bsd_eviction_pressure": 0, "semantic_deviation": 0
      }
    }
  },
  "circuit_breaker": {
    "enabled": true, "elevated_threshold": 0.5, "elevated_sustained": 2,
    "deescalate_sustained": 5,
    "step_up_enabled": true, "step_up_contextual": true,
    "step_up_at_level": "elevated",
    "step_up_on_inadmissible": $2,
    "output_admissibility": {
      "enabled": true, "threshold": 0.60, "action": "quarantine",
      "inadmissible_history": "exclude", "max_quarantine_streak": 0
    }
  },
  "agents": { "developer": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$1",
      "api_key_env": "FAKE_KEY_CHALFAIL", "max_tokens": 100, "max_turns": 20 } }
}
EOF
}
FLOOR_FIXTURE='{"responses": [
  {"content": "def divide(a, b): return a / b  # implemented divide", "output_tokens": 30},
  {"content": "def divide(a, b): return a / b  # fix attempt one", "output_tokens": 30},
  {"content": "def divide(a, b): return a / b  # fix attempt two", "output_tokens": 30},
  {"content": "def divide(a, b): return a / b  # fix attempt three", "output_tokens": 30},
  {"content": "The divide test failed because divide raised ZeroDivisionError instead of ValueError for a zero divisor. I will catch ZeroDivisionError and raise ValueError, then proceed.", "output_tokens": 40},
  {"content": "def divide(a, b): return a / b  # final version", "output_tokens": 30}
]}'
FLOOR_NAAB='use agent
main {
    let h = agent.create("developer")
    let r1 = agent.send(h, "implement divide")
    let _a = agent.record_validation(h, false, "FAILED test_divide_by_zero: divide raised ZeroDivisionError, expected ValueError for zero divisor")
    let r2 = agent.send(h, "fix the divide operation")
    let _b = agent.record_validation(h, false, "FAILED test_divide_by_zero: divide raised ZeroDivisionError, expected ValueError for zero divisor")
    let r3 = agent.send(h, "fix the divide operation again")
    let _c = agent.record_validation(h, false, "FAILED test_divide_by_zero: divide raised ZeroDivisionError, expected ValueError for zero divisor")
    let r4 = agent.send(h, "fix the divide operation once more")
    print("COH_AFTER_FAILS=" + string(agent.coherence(h)))
    let r5 = agent.send(h, "add the divide docstring")
    print("SEND5_OK|coh=" + string(agent.coherence(h)))
}'
# F-on: flag enabled — challenge fires, passes, coherence recovers
WDIR="$TEST_TMP/f_on"; mkdir -p "$WDIR"
printf '%s' "$FLOOR_FIXTURE" > "$WDIR/fixture.json"
start_stub "$WDIR/fixture.json" "$WDIR" || skip "F-01" "stub failed"
if [ -n "$STUB_PID" ]; then
mk_govern_floor "$STUB_PORT" true > "$WDIR/govern.json"; sign_govern "$WDIR"
printf '%s' "$FLOOR_NAAB" > "$WDIR/test.naab"
OUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1); EXIT_F=$?
stop_stub
COH_F=$(echo "$OUT" | grep COH_AFTER_FAILS | sed 's/.*=//')
if [ -n "${COH_F:-}" ] && awk "BEGIN{exit !($COH_F < 0.60)}"; then
    pass "F-01" "Staging valid: coherence below OA threshold after 3 failed validations ($COH_F)"
else
    fail "F-01" "Staging broken" "coherence=$COH_F $(echo "$OUT" | tail -2)"
fi
CHAL_F=$(grep '"event_type":"AGENT_CHALLENGE_PASS"' "$WDIR/tele.jsonl" 2>/dev/null | head -1)
if [ -n "$CHAL_F" ]; then
    pass "F-02" "Coherence-floor trigger fired a challenge with level NORMAL"
else
    fail "F-02" "No challenge despite sub-OA coherence + flag on" "$(grep -c GOVERNANCE_LEVEL_CHANGE "$WDIR/tele.jsonl" 2>/dev/null) level changes"
fi
if [ "$EXIT_F" -eq 0 ] && echo "$OUT" | grep -q "SEND5_OK"; then
    RECOV=$(echo "$OUT" | grep SEND5_OK | sed 's/.*coh=//')
    if awk "BEGIN{exit !($RECOV > $COH_F)}"; then
        pass "F-03" "Challenge pass recovered coherence ($COH_F -> $RECOV); run completed"
    else
        fail "F-03" "No recovery after challenge pass" "before=$COH_F after=$RECOV"
    fi
else
    fail "F-03" "Run did not complete" "exit=$EXIT_F"
fi
fi
# F-off: control — same staging, flag absent (default false): no challenge
WDIR="$TEST_TMP/f_off"; mkdir -p "$WDIR"
printf '%s' "$FLOOR_FIXTURE" > "$WDIR/fixture.json"
start_stub "$WDIR/fixture.json" "$WDIR" || skip "F-04" "stub failed"
if [ -n "$STUB_PID" ]; then
mk_govern_floor "$STUB_PORT" false > "$WDIR/govern.json"; sign_govern "$WDIR"
printf '%s' "$FLOOR_NAAB" > "$WDIR/test.naab"
OUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1) || true
stop_stub
if ! grep -qE '"event_type":"AGENT_CHALLENGE_(PASS|FAIL)"' "$WDIR/tele.jsonl" 2>/dev/null; then
    pass "F-04" "Control: no challenge without the flag (default-off preserved)"
else
    fail "F-04" "Challenge fired without the flag"
fi
fi

# ============================================================
# Group G: disabling step_up_on_inadmissible mid-run = ratchet violation
# ============================================================
echo -e "${CYAN}--- Group G: disabling the floor trigger is a ratchet violation ---${NC}"
IS_WINDOWS_G=false
if [[ "$(uname -s)" == MINGW* ]] || [[ "$(uname -s)" == MSYS* ]] || [[ -n "${WINDIR:-}" ]]; then IS_WINDOWS_G=true; fi
if $IS_WINDOWS_G; then
    skip "G-01" "mid-run file swap requires POSIX file semantics"
else
WDIR="$TEST_TMP/g"; mkdir -p "$WDIR"
printf '%s' "$FLOOR_FIXTURE" > "$WDIR/fixture.json"
start_stub "$WDIR/fixture.json" "$WDIR" || skip "G-01" "stub failed"
if [ -n "$STUB_PID" ]; then
presign_g() {
    local pdir="$TEST_TMP/.presign_g"; mkdir -p "$pdir"
    printf '%s' "$1" > "$pdir/govern.json"; sign_govern "$pdir"
    cat "$pdir/govern.json.sig" 2>/dev/null || echo ""; rm -rf "$pdir"
}
LOOSE_G="{\"version\":\"5.0\",\"mode\":\"enforce\",\"security\":{\"sandbox_level\":\"elevated\"},\"telemetry\":{\"enabled\":true,\"output_file\":\"tele.jsonl\"},\"behavioral_sequences\":{\"enabled\":true},\"context_drift\":{\"enabled\":true,\"level\":\"advisory\",\"check_interval_turns\":1},\"circuit_breaker\":{\"enabled\":true,\"step_up_enabled\":true,\"step_up_on_inadmissible\":false},\"capabilities\":{\"shell\":{\"enabled\":true}},\"agents\":{\"developer\":{\"provider\":\"gemini\",\"model\":\"stub-model\",\"api_base\":\"http://127.0.0.1:$STUB_PORT\",\"api_key_env\":\"FAKE_KEY_CHALFAIL\",\"max_tokens\":100,\"max_turns\":20}}}"
LOOSE_G_SIG=$(presign_g "$LOOSE_G")
if [ -z "$LOOSE_G_SIG" ]; then skip "G-01" "presign failed"; stop_stub; else
export NAAB_LOOSE_G_JSON="$LOOSE_G" NAAB_LOOSE_G_SIG="$LOOSE_G_SIG"
cat > "$WDIR/govern.json" <<EOF
{
  "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl" },
  "behavioral_sequences": { "enabled": true },
  "context_drift": { "enabled": true, "level": "advisory", "check_interval_turns": 1 },
  "circuit_breaker": { "enabled": true, "step_up_enabled": true, "step_up_on_inadmissible": true },
  "capabilities": { "shell": { "enabled": true } },
  "agents": { "developer": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT",
      "api_key_env": "FAKE_KEY_CHALFAIL", "max_tokens": 100, "max_turns": 20 } }
}
EOF
sign_govern "$WDIR"
cat > "$WDIR/test.naab" <<'EOF'
use agent
main {
    let h = agent.create("developer")
    let r1 = agent.send(h, "implement divide")
    let _ = <<shell
sleep 1.2
printf '%s' "$NAAB_LOOSE_G_JSON" > govern.json
printf '%s' "$NAAB_LOOSE_G_SIG" > govern.json.sig
>>
    let r2 = agent.send(h, "fix the divide operation")
    print("DONE")
}
EOF
OUT=$(cd "$WDIR" && timeout 40s "$NAAB" test.naab 2>"$WDIR/stderr.txt") || true
stop_stub
if grep -q "ratchet violation" "$WDIR/stderr.txt" 2>/dev/null && grep -q "step_up_on_inadmissible" "$WDIR/stderr.txt" 2>/dev/null; then
    pass "G-01" "mid-run disable of step_up_on_inadmissible rejected as ratchet violation"
else
    fail "G-01" "mid-run disable not rejected" "$(grep -i 'ratchet\|reload' "$WDIR/stderr.txt" 2>/dev/null | head -2)"
fi
fi
fi
fi

# ============================================================
echo ""
echo -e "${CYAN}+==============================================================+${NC}"
TOTAL=$((PASS_COUNT + FAIL_COUNT + SKIP_COUNT))
echo -e "  Total: $TOTAL | ${GREEN}Pass: $PASS_COUNT${NC} | ${RED}Fail: $FAIL_COUNT${NC} | ${YELLOW}Skip: $SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then echo -e "${RED}Failures:${NC}$FAILURES"; exit 1; fi
exit 0
