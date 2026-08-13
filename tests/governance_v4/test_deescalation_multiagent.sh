#!/usr/bin/env bash
# ============================================================
# test_deescalation_multiagent.sh — de-escalation with interleaved handles
#
# The governance level is engine-global, but living-script-style runs share it
# across many handles. Pre-fix, the calm counter (deescalate_calm_turns_) was
# advanced by ANY handle's calm turn — two interleaved turns from a
# well-behaved sibling stepped down the scrutiny a still-degraded agent had
# earned. Now calm turns count only when they come from the handle whose
# pressure raised/held the level (deescalate_pressure_handle_).
#
# Same deterministic pressure driver as test_deescalation_hysteresis.sh:
# only instruction_recall enabled; checkpoint weights put ALL weight on
# signal_density (divisor 1) so composite is 1.0 when the signal fires and
# 0.0 on clean turns. Stub responses are consumed in send order, so the
# fixture defines which handle's turn is pressure vs calm.
#
# NOTE: the FIRST analyzed turn of every handle fires instruction_recall
# regardless of response content (keyword bookkeeping lands after that
# turn's CDD pass). The timeline therefore burns each handle's first turn
# as a deliberate pressure turn and runs the sibling-calm experiment on
# later turns only.
#
# Timeline (elevated_sustained=1, deescalate_sustained=2):
#   send 1 (bravo): first turn  -> fires  -> ELEVATED (bravo is pressure handle)
#   send 2 (bravo): clean echo  -> calm 1 -> stays ELEVATED
#   send 3 (bravo): clean echo  -> calm 2 -> NORMAL (own-calm de-escalation works)
#   send 4 (alpha): first turn  -> fires  -> ELEVATED (alpha is pressure handle)
#   send 5 (bravo): clean echo  -> sibling calm -> must STAY ELEVATED
#   send 6 (bravo): clean echo  -> sibling calm -> must STAY ELEVATED
#                                  (pre-fix: global counter hits 2 -> NORMAL)
#   send 7 (alpha): clean echo  -> calm 1 -> stays ELEVATED (pre-fix: already normal)
#   send 8 (alpha): clean echo  -> calm 2 -> steps down to NORMAL
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_deescalation_multiagent.sh"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/deesc-multi-$$"

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
export FAKE_KEY_DEESC_MULTI="fake-key-deescalation-multiagent-test"

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

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|    De-escalation hysteresis with interleaved agent handles   |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

WDIR="$TEST_TMP/multi"; mkdir -p "$WDIR"
# Clean echoes repeat the prompt's exact tokens ("reconcile ledger quarterly
# totals balance") so instruction_recall stays satisfied on calm turns.
cat > "$WDIR/fixture.json" << 'EOF'
{"responses": [
  {"content": "meandering filler paragraph about nothing important whatsoever", "output_tokens": 20},
  {"content": "reconcile ledger quarterly totals balance work proceeding steadily", "output_tokens": 20},
  {"content": "reconcile ledger quarterly totals balance almost finished now", "output_tokens": 20},
  {"content": "unrelated rambling filler avoiding every requested subject entirely", "output_tokens": 20},
  {"content": "reconcile ledger quarterly totals balance verified once more", "output_tokens": 20},
  {"content": "reconcile ledger quarterly totals balance still holding steady", "output_tokens": 20},
  {"content": "reconcile ledger quarterly totals balance resumed after review", "output_tokens": 20},
  {"content": "reconcile ledger quarterly totals balance completed successfully today", "output_tokens": 20}
]}
EOF
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "M-00" "stub failed to start"; exit 1; }

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
        "alpha": {
            "provider": "gemini", "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": "FAKE_KEY_DEESC_MULTI", "max_tokens": 100, "max_turns": 20
        },
        "bravo": {
            "provider": "gemini", "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": "FAKE_KEY_DEESC_MULTI", "max_tokens": 100, "max_turns": 20
        }
    }
}
GOVEOF
sign_govern "$WDIR"

cat > "$WDIR/test.naab" << 'NAABEOF'
use agent
main {
    let a = agent.create("alpha")
    let b = agent.create("bravo")
    let r1 = agent.send(b, "reconcile ledger quarterly totals balance")
    let r2 = agent.send(b, "reconcile ledger quarterly totals balance")
    let r3 = agent.send(b, "reconcile ledger quarterly totals balance")
    let r4 = agent.send(a, "reconcile ledger quarterly totals balance")
    let r5 = agent.send(b, "reconcile ledger quarterly totals balance")
    let r6 = agent.send(b, "reconcile ledger quarterly totals balance")
    let r7 = agent.send(a, "reconcile ledger quarterly totals balance")
    let r8 = agent.send(a, "reconcile ledger quarterly totals balance")
    print("DONE")
}
NAABEOF

OUTPUT=$(cd "$WDIR" && timeout 90s "$NAAB" test.naab 2>&1) || true
stop_stub

echo "$OUTPUT" | grep -q "DONE" && pass "M-01" "8 interleaved sends complete" \
    || fail "M-01" "sends did not complete" "$(echo "$OUTPUT" | head -3)"

L1=$(cdd_level "$WDIR/telemetry.jsonl" 1)
L3=$(cdd_level "$WDIR/telemetry.jsonl" 3)
L4=$(cdd_level "$WDIR/telemetry.jsonl" 4)
L5=$(cdd_level "$WDIR/telemetry.jsonl" 5)
L6=$(cdd_level "$WDIR/telemetry.jsonl" 6)
L7=$(cdd_level "$WDIR/telemetry.jsonl" 7)
L8=$(cdd_level "$WDIR/telemetry.jsonl" 8)

if [ "$L1" = "elevated" ]; then
    pass "M-02" "bravo's pressure turn escalates immediately"
else
    fail "M-02" "Fast escalation broken" "turn1 level=$L1"
fi
if [ "$L3" = "normal" ]; then
    pass "M-03" "bravo de-escalates via its OWN two calm turns (baseline works)"
else
    fail "M-03" "Own-calm de-escalation broken" "turn3 level=$L3"
fi
if [ "$L4" = "elevated" ]; then
    pass "M-04" "alpha's pressure turn re-escalates immediately"
else
    fail "M-04" "Re-escalation broken" "turn4 level=$L4"
fi
if [ "$L5" = "elevated" ]; then
    pass "M-05" "bravo's first calm turn holds alpha's ELEVATED"
else
    fail "M-05" "Level lost on sibling's first calm turn" "turn5 level=$L5"
fi
if [ "$L6" = "elevated" ]; then
    pass "M-06" "bravo's second calm turn does NOT de-escalate alpha's level"
else
    fail "M-06" "Calm sibling drained the calm counter (pre-fix behavior)" "turn6 level=$L6"
fi
if [ "$L7" = "elevated" ]; then
    pass "M-07" "alpha's first calm turn stays ELEVATED (hysteresis intact)"
else
    fail "M-07" "Level wrong on pressure handle's first calm turn" "turn7 level=$L7"
fi
if [ "$L8" = "normal" ]; then
    pass "M-08" "alpha's second consecutive calm turn steps down to NORMAL"
else
    fail "M-08" "De-escalation by the pressure handle broken" "turn8 level=$L8"
fi

echo ""
echo "================================================"
echo "  PASS: $PASS_COUNT  FAIL: $FAIL_COUNT  SKIP: $SKIP_COUNT"
echo "================================================"
[ -n "$FAILURES" ] && echo -e "Failures:$FAILURES"

exit "$FAIL_COUNT"
