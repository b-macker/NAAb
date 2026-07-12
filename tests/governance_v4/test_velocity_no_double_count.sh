#!/usr/bin/env bash
# ============================================================
# test_velocity_no_double_count.sh — S6 coherence_velocity is detection-only
#
# Coherence only changes via signal penalties/recovery, so S6's velocity is
# exactly last turn's net penalty. Pre-fix it subtracted another 0.12 on the
# turn AFTER a >0.15-penalty turn — even when that turn's content was clean —
# and each S6 penalty fed the next velocity reading (self-sustaining cascade,
# the dominant amplifier in the July 11 living-script run). Now S6 fires
# (telemetry/dashboard/pressure) but never subtracts coherence.
#
# Scenario (instruction_recall + response_repetition + coherence_velocity):
#   send 1: filler ignoring prompt        -> recall (0.08)          coh 0.92
#   send 2: identical filler              -> recall+repetition 0.23 coh 0.69
#   send 3: clean response echoing prompt -> velocity -0.23 detected,
#           NO penalty: coherence stays 0.69
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/veldc-$$"

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
export FAKE_KEY_VEL_TEST="fake-key-velocity-test"

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

cdd_turn_line() { grep '"event_type":"CDD_TURN"' "$1" 2>/dev/null | sed -n "${2}p"; }

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|     coherence_velocity (S6) — no penalty double-counting     |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

WDIR="$TEST_TMP/vel"; mkdir -p "$WDIR"
cat > "$WDIR/fixture.json" << 'EOF'
{"responses": [
  {"content": "meandering filler paragraph about nothing important whatsoever today", "output_tokens": 20},
  {"content": "meandering filler paragraph about nothing important whatsoever today", "output_tokens": 20},
  {"content": "resuming ledger reconciliation quarterly totals verified balance confirmed", "output_tokens": 20}
]}
EOF
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "V-00" "stub failed to start"; exit 1; }

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
            "coherence_velocity": true, "capability_underutilization": false,
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
            "api_key_env": "FAKE_KEY_VEL_TEST", "max_tokens": 100, "max_turns": 20
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
    let r3 = agent.send(h, "reconcile ledger quarterly totals balance")
    print("DONE")
}
NAABEOF

OUTPUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1) || true
stop_stub

echo "$OUTPUT" | grep -q "DONE" && pass "V-01" "3 sends complete" \
    || fail "V-01" "sends did not complete" "$(echo "$OUTPUT" | head -3)"

T2=$(cdd_turn_line "$WDIR/telemetry.jsonl" 2)
T3=$(cdd_turn_line "$WDIR/telemetry.jsonl" 3)

# Send 2 took recall (0.08) + repetition (0.15): coherence 0.69
if echo "$T2" | grep -q '"coherence":"0.6900"'; then
    pass "V-02" "Send 2 penalized 0.23 by content signals (coherence 0.69)"
else
    fail "V-02" "Send-2 coherence unexpected" "$T2"
fi

# Send 3 is clean: velocity -0.23 must be DETECTED but NOT penalized
if echo "$T3" | grep -q '"velocity":"-0.2300"'; then
    pass "V-03" "Velocity -0.23 still reported in CDD_TURN telemetry"
else
    fail "V-03" "Velocity not reported" "$T3"
fi
if echo "$T3" | grep -q '"coherence":"0.6900"'; then
    pass "V-04" "Clean turn NOT penalized: coherence unchanged at 0.69"
else
    fail "V-04" "Clean turn was penalized (velocity double-count)" "$T3"
fi
if echo "$T3" | grep -q '"signals_detail":"[^"]*coherence_velocity'; then
    pass "V-05" "S6 firing still visible in signals_detail (detection preserved)"
else
    fail "V-05" "S6 detection lost from telemetry" "$T3"
fi
if echo "$T3" | grep -q '"penalties_detail":"[^"]*coherence_velocity'; then
    fail "V-06" "S6 still applies a penalty" "$T3"
else
    pass "V-06" "No coherence_velocity entry in penalties_detail"
fi

echo ""
echo "================================================"
echo "  PASS: $PASS_COUNT  FAIL: $FAIL_COUNT  SKIP: $SKIP_COUNT"
echo "================================================"
[ -n "$FAILURES" ] && echo -e "Failures:$FAILURES"

exit "$FAIL_COUNT"
