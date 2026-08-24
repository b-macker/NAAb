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
#   sends 1-3: clean, distinct, echo the prompt -> nothing fires; the adaptive
#              baseline completes on a CALIBRATED-CLEAN window
#   send 4: filler ignoring prompt        -> recall (0.08)          coh 0.92
#   send 5: identical filler              -> recall+repetition 0.23 coh 0.69
#   send 6: clean response echoing prompt -> velocity -0.23 detected,
#           NO penalty: coherence stays 0.69
#
# WHY THE THREE WARM-UP SENDS EXIST
#
# adaptive_baseline_enabled defaults TRUE. `in_baseline` suppresses every
# STATISTICAL signal's penalty until the window completes, and this scenario
# was originally 3 sends against a window of 5 — so instruction_recall never
# charged, send 2 took only the objective response_repetition (0.15), and
# velocity came out at exactly -0.1500.
#
# That is not merely a different number. The S6 trigger is
# `velocity < thresholds.velocity_drop` with velocity_drop = -0.15, and
# -0.1500 is NOT < -0.15. Re-pointing the assertions at the observed values
# would have produced a suite that passes without ever crossing the threshold
# the regression is about — green against a build with the double-count bug
# put back. The vacuity check at the bottom of this file is what catches that.
#
# So the run is extended rather than the constants relaxed. The warm-up must be
# CLEAN: the baseline learns each signal's rate, and a signal that fires during
# the window teaches the baseline that firing is normal (threshold becomes
# mean + 2*max(stddev, 0.1)), which absorbs the very firing this test needs.
# With a clean window the threshold is 0 + 2*0.1 = 0.2, the first post-baseline
# firing has rate 1.0, and it charges FULL weight — restoring the original
# 0.92 / 0.69 / -0.23 arithmetic exactly, three turns later.
#
# adaptive_baseline_enabled is deliberately NOT set in the config below: this
# suite must exercise the DEFAULT path. Only the window is pinned, so the test
# does not silently re-break when the default window is retuned.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_velocity_no_double_count.sh"

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

# This suite carried its own copy of start_stub: one port pick, no bind check,
# and a flat 5s ceiling for READY. On a loaded runner that is not enough — a
# collision or a lingering TIME_WAIT socket leaves the stub dead, python3
# startup plus bind can exceed 5s, and either way V-00 skips and the suite
# exits 1 for a reason unrelated to what it measures. That is exactly what
# turned master red on d6e3bd4: "SKIP [V-00] stub failed to start", on a commit
# that changed nothing this suite touches.
#
# The hardened launcher retries three ports, waits 30s, notices a dead child,
# and prints the stub log tail on failure. Sourcing it deletes this copy rather
# than fixing it, so the next fix reaches here too.
source "$SCRIPT_DIR/../helpers/stub_launch.sh"

cdd_turn_line() { grep '"event_type":"CDD_TURN"' "$1" 2>/dev/null | sed -n "${2}p"; }

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|     coherence_velocity (S6) — no penalty double-counting     |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

WDIR="$TEST_TMP/vel"; mkdir -p "$WDIR"
cat > "$WDIR/fixture.json" << 'EOF'
{"responses": [
  {"content": "reconcile the ledger: quarterly totals agree and the balance ties to source", "output_tokens": 20},
  {"content": "reconcile pass two on the ledger, quarterly totals recomputed, balance holds", "output_tokens": 20},
  {"content": "ledger reconcile complete for quarterly totals with the closing balance signed", "output_tokens": 20},
  {"content": "meandering filler paragraph about nothing important whatsoever today", "output_tokens": 20},
  {"content": "meandering filler paragraph about nothing important whatsoever today", "output_tokens": 20},
  {"content": "resuming ledger reconcile quarterly totals verified balance confirmed", "output_tokens": 20}
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
        "adaptive_baseline_window": 3,
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
    let i = 0
    while i < 6 {
        let r = agent.send(h, "reconcile ledger quarterly totals balance")
        i = i + 1
    }
    print("DONE")
}
NAABEOF

OUTPUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1) || true
stop_stub

echo "$OUTPUT" | grep -q "DONE" && pass "V-01" "6 sends complete" \
    || fail "V-01" "sends did not complete" "$(echo "$OUTPUT" | head -3)"

T3=$(cdd_turn_line "$WDIR/telemetry.jsonl" 3)
T5=$(cdd_turn_line "$WDIR/telemetry.jsonl" 5)
T6=$(cdd_turn_line "$WDIR/telemetry.jsonl" 6)

# The warm-up must have been CLEAN and the baseline must have COMPLETED on it.
# Without this the later assertions still hold for the wrong reason: a warm-up
# that fired recall teaches the baseline to absorb it, and coherence lands
# somewhere else entirely.
if echo "$T3" | grep -q '"coherence":"1.0000"'; then
    pass "V-01b" "Warm-up window is clean (coherence 1.0 at end of baseline)"
else
    fail "V-01b" "Warm-up fired a signal — baseline is contaminated" "$T3"
fi
if echo "$T5" | grep -q '"baseline_state":"complete"'; then
    pass "V-01c" "Baseline completed before the drift turns"
else
    fail "V-01c" "Still calibrating at send 5 — statistical signals cannot charge" "$T5"
fi

# Send 5 took recall (0.08) + repetition (0.15): coherence 0.69
if echo "$T5" | grep -q '"coherence":"0.6900"'; then
    pass "V-02" "Send 5 penalized 0.23 by content signals (coherence 0.69)"
else
    fail "V-02" "Send-5 coherence unexpected" "$T5"
fi

# Send 6 is clean: velocity -0.23 must be DETECTED but NOT penalized.
# -0.23 clears velocity_drop (-0.15) with margin; see the header on why this
# margin is the point and not an incidental value.
if echo "$T6" | grep -q '"velocity":"-0.2300"'; then
    pass "V-03" "Velocity -0.23 still reported in CDD_TURN telemetry"
else
    fail "V-03" "Velocity not reported" "$T6"
fi
if echo "$T6" | grep -q '"coherence":"0.6900"'; then
    pass "V-04" "Clean turn NOT penalized: coherence unchanged at 0.69"
else
    fail "V-04" "Clean turn was penalized (velocity double-count)" "$T6"
fi
if echo "$T6" | grep -q '"signals_detail":"[^"]*coherence_velocity'; then
    pass "V-05" "S6 firing still visible in signals_detail (detection preserved)"
else
    fail "V-05" "S6 detection lost from telemetry" "$T6"
fi
if echo "$T6" | grep -q '"penalties_detail":"[^"]*coherence_velocity'; then
    fail "V-06" "S6 still applies a penalty" "$T6"
else
    pass "V-06" "No coherence_velocity entry in penalties_detail"
fi

echo ""
echo "================================================"
echo "  PASS: $PASS_COUNT  FAIL: $FAIL_COUNT  SKIP: $SKIP_COUNT"
echo "================================================"
[ -n "$FAILURES" ] && echo -e "Failures:$FAILURES"

exit "$FAIL_COUNT"
