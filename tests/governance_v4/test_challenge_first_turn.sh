#!/usr/bin/env bash
# ============================================================
# test_challenge_first_turn.sh — challenges grade only DELIVERED information
#
# Pre-fix, the pending prompt's keywords entered instruction_history BEFORE
# the step-up gate, so a fresh handle's first challenge was the "instruction"
# type, graded against keywords of a message the agent never received (the
# challenge blocks that very send, and handle history is empty) —
# structurally unpassable. Keyword bookkeeping now runs after the gates, so
# a fresh handle falls through to the mandate challenge, which is gradeable
# (the agent has its system prompt).
#
# Setup: handle "alpha" fires the only enabled signal once to push the
# engine-global level to ELEVATED (signal-density pressure driver), then a
# FRESH handle "bravo" sends its first message and gets challenged.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/chalft-$$"

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
export FAKE_KEY_CHALFT_TEST="fake-key-challenge-first-turn-test"

sign_govern() {
    (cd "$1" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
}

start_stub() {  # $1=fixture $2=workdir
    STUB_PORT=$(( (RANDOM % 20000) + 20000 ))
    python3 "$SCRIPT_DIR/../helpers/agent_stub.py" "$STUB_PORT" "$1" "$2" > "$2/stub.log" 2>&1 &
    STUB_PID=$!
    for _ in $(seq 1 50); do
        grep -q READY "$2/stub.log" 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}
stop_stub() { [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null; wait "$STUB_PID" 2>/dev/null; STUB_PID=""; }

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|     Step-up challenge on a fresh handle grades delivered     |${NC}"
echo -e "${CYAN}|     information only (mandate type, passable)                |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

WDIR="$TEST_TMP/ft"; mkdir -p "$WDIR"
# Order: alpha's response (fires recall -> ELEVATED), bravo's challenge
# answer (mandate-aligned), bravo's real answer.
cat > "$WDIR/fixture.json" << 'EOF'
{"responses": [
  {"content": "meandering filler paragraph about nothing important whatsoever", "output_tokens": 20},
  {"content": "I remain focused on my assigned task to summarize quarterly ledger data right now", "output_tokens": 20},
  {"content": "quarterly ledger data summarized and totals verified", "output_tokens": 20}
]}
EOF
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "F-00" "stub failed to start"; exit 1; }

cat > "$WDIR/govern.json" << GOVEOF
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "telemetry": { "enabled": true, "output_file": "telemetry.jsonl" },
    "behavioral_sequences": { "enabled": true },
    "context_drift": {
        "enabled": true, "level": "advisory", "check_interval_turns": 1,
        "coherence_threshold": 0.05,
        "signals": {
            "circular_actions": false, "repeated_failures": false, "scope_creep": false,
            "intent_contradictions": false, "vocabulary_contraction": false,
            "coherence_velocity": false, "capability_underutilization": false,
            "response_quality": false, "thinking_collapse": false,
            "semantic_stability": false, "mandate_alignment": false,
            "context_growth": false, "instruction_recall": true, "plan_drift": false,
            "entity_consistency": false, "instruction_conflict": true,
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
        "deescalate_sustained": 5,
        "step_up_enabled": true,
        "step_up_contextual": true,
        "step_up_at_level": "elevated"
    },
    "agents": {
        "alpha": {
            "provider": "gemini", "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": "FAKE_KEY_CHALFT_TEST", "max_tokens": 100, "max_turns": 20
        },
        "bravo": {
            "provider": "gemini", "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": "FAKE_KEY_CHALFT_TEST", "max_tokens": 100, "max_turns": 20,
            "system_prompt": "summarize quarterly ledger data"
        }
    }
}
GOVEOF
sign_govern "$WDIR"

cat > "$WDIR/test.naab" << 'NAABEOF'
use agent
main {
    let a = agent.create("alpha")
    let r1 = agent.send(a, "reconcile ledger quarterly totals balance")
    // Engine level is now ELEVATED. A brand-new handle's first send must be
    // challenged with the MANDATE type (system prompt is the only delivered
    // information), not quizzed on the never-delivered pending prompt.
    let b = agent.create("bravo")
    let r2 = agent.send(b, "please summarize the quarterly ledger data")
    print("B_CONTENT=" + string(r2.get("content")))
}
NAABEOF

OUTPUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1) || true
stop_stub

TELE="$WDIR/telemetry.jsonl"
CHAL=$(grep -E '"event_type":"AGENT_CHALLENGE_(PASS|FAIL)"' "$TELE" 2>/dev/null | head -1)

if [ -n "$CHAL" ]; then
    pass "F-01" "Fresh handle's first send triggered a step-up challenge"
else
    fail "F-01" "No challenge fired for fresh handle at elevated level" "$(grep -c CDD_TURN "$TELE" 2>/dev/null) CDD turns"
fi
if echo "$CHAL" | grep -q '"challenge_type":"mandate"'; then
    pass "F-02" "Challenge type is 'mandate' (delivered info only; pre-fix: 'instruction')"
else
    fail "F-02" "Challenge quizzed undelivered information" "$CHAL"
fi
if echo "$CHAL" | grep -q '"event_type":"AGENT_CHALLENGE_PASS"'; then
    pass "F-03" "Mandate challenge is passable on the first turn"
else
    fail "F-03" "First-turn challenge failed (structurally unpassable?)" "$CHAL"
fi
if echo "$OUTPUT" | grep -q "B_CONTENT=quarterly ledger data summarized"; then
    pass "F-04" "Fresh handle's send completed after passing the challenge"
else
    fail "F-04" "Send did not complete" "$(echo "$OUTPUT" | tail -3)"
fi
# Telemetry attribution: challenge events carry handle_id and config_name
# (previously only under the non-standard "agent" key).
if echo "$CHAL" | grep -q '"config_name":"bravo"'; then
    pass "F-05" "Challenge telemetry includes config_name attribution"
else
    fail "F-05" "Challenge telemetry missing config_name" "$CHAL"
fi
if echo "$CHAL" | grep -qE '"handle_id":"[0-9]+"'; then
    pass "F-06" "Challenge telemetry includes handle_id"
else
    fail "F-06" "Challenge telemetry missing handle_id" "$CHAL"
fi

echo ""
echo "================================================"
echo "  PASS: $PASS_COUNT  FAIL: $FAIL_COUNT  SKIP: $SKIP_COUNT"
echo "================================================"
[ -n "$FAILURES" ] && echo -e "Failures:$FAILURES"

exit "$FAIL_COUNT"
