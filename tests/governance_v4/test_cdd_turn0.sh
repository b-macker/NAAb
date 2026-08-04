#!/usr/bin/env bash
# ============================================================
# test_cdd_turn0.sh — CDD analyzes the first send (turn 0)
#
# DriftState.last_checked_turn used to initialize to 0 while agent event
# turns start at 0, so `0 - 0 < check_interval_turns` silently skipped the
# first send of every handle: no fingerprint, no recall check, no entity
# baseline. With the fix (init -1) the first response is analyzed, and the
# full-content S21 fingerprint (previously first-200-chars) detects a
# verbatim duplicate on the SECOND send.
#
# T0-A: instruction_recall evaluated on the very first send
# T0-B: identical send-1/send-2 responses trip response_repetition at send 2
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_cdd_turn0.sh"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/cddturn0-$$"

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
export FAKE_KEY_T0_TEST="fake-key-cdd-turn0-test"

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

# Nth CDD_TURN line (1-based) from a telemetry file
cdd_turn_line() { grep '"event_type":"CDD_TURN"' "$1" 2>/dev/null | sed -n "${2}p"; }

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|      CDD turn-0 analysis + full-content S21 fingerprint      |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

WDIR="$TEST_TMP/t0"; mkdir -p "$WDIR"
cat > "$WDIR/fixture.json" << 'EOF'
{"responses": [
  {"content": "meandering filler paragraph about nothing important whatsoever today", "output_tokens": 20},
  {"content": "meandering filler paragraph about nothing important whatsoever today", "output_tokens": 20}
]}
EOF
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "T0-00" "stub failed to start"; exit 1; }

cat > "$WDIR/govern.json" << GOVEOF
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "telemetry": { "enabled": true, "output_file": "telemetry.jsonl" },
    "behavioral_sequences": { "enabled": true },
    "context_drift": { "enabled": true, "level": "advisory", "check_interval_turns": 1,
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
            "response_repetition": true
        } },
    "agents": {
        "worker": {
            "provider": "gemini", "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": "FAKE_KEY_T0_TEST", "max_tokens": 100, "max_turns": 20
        }
    }
}
GOVEOF
sign_govern "$WDIR"

cat > "$WDIR/test.naab" << 'NAABEOF'
use agent
main {
    let h = agent.create("worker")
    let r1 = agent.send(h, "reconcile ledger quarterly totals balance")
    let r2 = agent.send(h, "reconcile ledger quarterly totals balance")
    print("DONE")
}
NAABEOF

OUTPUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1) || true
stop_stub

if echo "$OUTPUT" | grep -q "DONE"; then
    pass "T0-01" "Two sends complete"
else
    fail "T0-01" "Sends did not complete" "$(echo "$OUTPUT" | head -3)"
fi

TURN1=$(cdd_turn_line "$WDIR/telemetry.jsonl" 1)
TURN2=$(cdd_turn_line "$WDIR/telemetry.jsonl" 2)

# T0-A: instruction_recall must fire on the FIRST send (response ignores prompt)
if echo "$TURN1" | grep -q '"signals_detail":"[^"]*instruction_recall'; then
    pass "T0-02" "instruction_recall evaluated on the first send (turn 0 analyzed)"
else
    fail "T0-02" "First send not analyzed by CDD" "$TURN1"
fi

# T0-B: identical second response must trip response_repetition
# (requires turn 0's fingerprint recorded AND full-content hashing)
if echo "$TURN2" | grep -q '"signals_detail":"[^"]*response_repetition'; then
    pass "T0-03" "Verbatim duplicate of the first response detected at send 2"
else
    fail "T0-03" "S21 missed duplicate of first response" "$TURN2"
fi
if echo "$TURN2" | grep -q '"response_repetition_count":"1"'; then
    pass "T0-04" "response_repetition_count reflects the duplicate"
else
    fail "T0-04" "repetition count wrong" "$TURN2"
fi

echo ""
echo "================================================"
echo "  PASS: $PASS_COUNT  FAIL: $FAIL_COUNT  SKIP: $SKIP_COUNT"
echo "================================================"
[ -n "$FAILURES" ] && echo -e "Failures:$FAILURES"

exit "$FAIL_COUNT"
