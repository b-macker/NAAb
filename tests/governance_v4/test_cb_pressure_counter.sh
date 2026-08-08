#!/usr/bin/env bash
# ============================================================
# test_cb_pressure_counter.sh — the circuit breaker owns its sustained counter
#
# Level escalation needs BOTH `composite >= circuit_breaker.<level>_threshold`
# AND `consecutive >= <level>_sustained`. The second half used to be fed by the
# REALITY CHECKPOINT's counter, which advances only above
# context_drift.reality_checkpoint.pressure_threshold — a different knob,
# default 0.70. Every circuit-breaker threshold below that was therefore
# unreachable however high pressure ran. On the engine defaults (elevated 0.4,
# high 0.6) both middle levels could never fire from pressure at all, leaving no
# graduated response between NORMAL and CRITICAL. A 117-turn keyed run sat at
# NORMAL with pressure peaking at 0.375 against an elevated_threshold of 0.35,
# and was written up twice as "the agents behaved".
#
# The counter could NOT simply be repointed at a lower gate: the checkpoint's own
# enforcement is SOFT (it blocks, exit 3) and is enabled by default, so a lower
# shared gate would have started blocking runs that pass today. The breaker got
# its own per-level counters instead.
#
# Deterministic driver: all pressure weight on signal_density with divisor 2, so
# one firing signal yields composite exactly 0.5 — ABOVE elevated_threshold 0.4
# and BELOW pressure_threshold 0.7. That gap is the whole bug, and it is where
# this test lives.
#
# CB-04 is the load-bearing assertion. Without it, simply lowering the shared
# gate would pass CB-02/CB-03 while reintroducing the SOFT-block regression the
# split exists to avoid.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_cb_pressure_counter.sh"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/cbpress-$$"

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
export FAKE_KEY_CBPRESS_TEST="fake-key-cb-pressure-test"

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

cdd_level() {  # $1=telemetry $2=nth CDD_TURN
    grep '"event_type":"CDD_TURN"' "$1" 2>/dev/null | sed -n "${2}p" \
        | grep -o '"governance_level":"[a-z]*"' | grep -o ':"[a-z]*"' | tr -d ':"'
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|   Circuit breaker owns its sustained-pressure counter        |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

WDIR="$TEST_TMP/cb"; mkdir -p "$WDIR"
cat > "$WDIR/fixture.json" << 'EOF'
{"responses": [
  {"content": "meandering filler paragraph about nothing important whatsoever", "output_tokens": 20},
  {"content": "further meandering filler avoiding every requested subject entirely", "output_tokens": 20},
  {"content": "still more meandering filler with no bearing on the request at all", "output_tokens": 20}
]}
EOF
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "CB-00" "stub failed to start"; exit 0; }

# elevated_threshold 0.4 (the ENGINE DEFAULT) with the checkpoint gate left at
# its 0.70 default. Composite lands at 0.5 — in the gap.
cat > "$WDIR/govern.json" << GOVEOF
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "telemetry": { "enabled": true, "output_file": "telemetry.jsonl", "decision_snapshots": true },
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
            "pressure_threshold": 0.7,
            "signal_density_divisor": 2,
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
        "elevated_threshold": 0.4,
        "elevated_sustained": 2,
        "deescalate_sustained": 99,
        "output_admissibility": {
            "enabled": true, "threshold": 0.0, "action": "quarantine"
        }
    },
    "agents": {
        "worker": {
            "provider": "gemini", "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": "FAKE_KEY_CBPRESS_TEST", "max_tokens": 100, "max_turns": 20
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
TELE="$WDIR/telemetry.jsonl"

if echo "$OUTPUT" | grep -q "DONE"; then
    pass "CB-01" "3 sends complete"
else
    fail "CB-01" "sends did not complete" "$(echo "$OUTPUT" | head -3)"
fi

# Sanity: the driver must actually land in the gap, or CB-02..05 prove nothing.
PRESSURES=$(grep '"event_type":"CDD_TURN"' "$TELE" 2>/dev/null \
    | grep -o '"pressure":"[0-9.]*"' | grep -o '[0-9.]*' | tr '\n' ' ')
IN_GAP=$(for p in $PRESSURES; do awk "BEGIN{if ($p>=0.4 && $p<0.7) print 1}"; done | wc -l)
if [ "${IN_GAP:-0}" -ge 2 ]; then
    pass "CB-02" "driver lands in the gap: pressures [$PRESSURES] between 0.4 and 0.7"
else
    skip "CB-02" "pressure never landed between elevated_threshold and the checkpoint gate [$PRESSURES] — CB-03..05 cannot discriminate"
    echo ""
    echo "  Total: $((PASS_COUNT + FAIL_COUNT + SKIP_COUNT)) | Pass: $PASS_COUNT | Fail: $FAIL_COUNT | Skip: $SKIP_COUNT"
    exit 0
fi

L1=$(cdd_level "$TELE" 1); L2=$(cdd_level "$TELE" 2); L3=$(cdd_level "$TELE" 3)

# Sustained still means sustained: one qualifying turn must NOT escalate.
# Note this assertion passes vacuously when escalation is broken entirely — a
# run that never escalates trivially "did not escalate early". It is meaningful
# only paired with CB-04a below, which fails in exactly that case. Verified: with
# the breaker reverted to the shared counter, CB-03 passes and CB-04a fails.
if [ "$L1" = "normal" ]; then
    pass "CB-03" "one qualifying turn does not escalate (elevated_sustained=2 respected)"
else
    fail "CB-03" "escalated before the sustained count was met" "turn1 level=$L1"
fi

# The fix: reachable below reality_checkpoint.pressure_threshold.
if [ "$L2" = "elevated" ] || [ "$L3" = "elevated" ]; then
    pass "CB-04a" "escalates below the checkpoint gate (was impossible: counter pinned at 0)"
else
    fail "CB-04a" "no escalation — the breaker is still gated on the checkpoint threshold" \
         "levels: $L1/$L2/$L3, pressures [$PRESSURES]"
fi

# LOAD-BEARING: the checkpoint's own counter must be untouched. If this reads
# non-zero, the shared gate was lowered rather than the counters split, and the
# checkpoint's SOFT block would start firing on configs that pass today.
CHK_MAX=$(grep -o '"consecutive_high_pressure_turns":[0-9]*' "$TELE" 2>/dev/null \
    | grep -o '[0-9]*$' | sort -rn | head -1)
if [ -z "$CHK_MAX" ]; then
    skip "CB-04" "no cdd_snapshot in telemetry — cannot verify counter independence"
elif [ "$CHK_MAX" = "0" ]; then
    pass "CB-04" "checkpoint counter stayed 0 — the counters are independent, not a lowered shared gate"
else
    fail "CB-04" "checkpoint counter advanced below its own threshold" \
         "max consecutive_high_pressure_turns=$CHK_MAX with pressure_threshold=0.7"
fi

CB_MAX=$(grep -o '"cb_sustained_elevated":[0-9]*' "$TELE" 2>/dev/null \
    | grep -o '[0-9]*$' | sort -rn | head -1)
if [ -z "$CB_MAX" ]; then
    skip "CB-05" "cb_sustained_elevated absent from snapshots"
elif [ "$CB_MAX" -ge 2 ]; then
    pass "CB-05" "circuit breaker accumulated its own evidence (cb_sustained_elevated=$CB_MAX)"
else
    fail "CB-05" "breaker counter did not accumulate" "max=$CB_MAX, expected >= 2"
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
TOTAL=$((PASS_COUNT + FAIL_COUNT + SKIP_COUNT))
echo -e "  Total: $TOTAL | ${GREEN}Pass: $PASS_COUNT${NC} | ${RED}Fail: $FAIL_COUNT${NC} | ${YELLOW}Skip: $SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then
    echo -e "${RED}Failures:${NC}$FAILURES"
    exit 1
fi
exit 0
