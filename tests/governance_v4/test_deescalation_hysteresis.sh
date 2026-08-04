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

cdd_level() {  # $1=telemetry file $2=nth CDD_TURN
    grep '"event_type":"CDD_TURN"' "$1" 2>/dev/null | sed -n "${2}p" \
        | grep -o '"governance_level":"[a-z]*"' | grep -o ':"[a-z]*"' | tr -d ':"'
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|         Governance level de-escalation hysteresis            |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

WDIR="$TEST_TMP/de"; mkdir -p "$WDIR"
cat > "$WDIR/fixture.json" << 'EOF'
{"responses": [
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
    let r1 = agent.send(h, "reconcile ledger quarterly totals balance")
    let r2 = agent.send(h, "reconcile ledger quarterly totals balance")
    let r3 = agent.send(h, "reconcile ledger quarterly totals balance")
    let r4 = agent.send(h, "reconcile ledger quarterly totals balance")
    print("DONE")
}
NAABEOF

OUTPUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1) || true
stop_stub

echo "$OUTPUT" | grep -q "DONE" && pass "D-01" "4 sends complete" \
    || fail "D-01" "sends did not complete" "$(echo "$OUTPUT" | head -3)"

L1=$(cdd_level "$WDIR/telemetry.jsonl" 1)
L2=$(cdd_level "$WDIR/telemetry.jsonl" 2)
L3=$(cdd_level "$WDIR/telemetry.jsonl" 3)
L4=$(cdd_level "$WDIR/telemetry.jsonl" 4)

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

echo ""
echo "================================================"
echo "  PASS: $PASS_COUNT  FAIL: $FAIL_COUNT  SKIP: $SKIP_COUNT"
echo "================================================"
[ -n "$FAILURES" ] && echo -e "Failures:$FAILURES"

exit "$FAIL_COUNT"
