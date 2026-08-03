#!/usr/bin/env bash
# ============================================================
# test_entity_window.sh — S15 entity_consistency windowed per-sighting model
#
# entity_context used to accumulate an unbounded per-entity union of every
# co-occurring keyword; Jaccard against a monotonically growing union decays
# toward zero, so ANY evolving agent eventually fired S15 every turn forever
# (reproduced pre-fix: fired from turn 6 onward with 50% vocabulary
# continuity). Now each entity keeps a bounded deque of per-sighting context
# sets and a contradiction means the current context matches NONE of them.
#
# E-A: coherent evolving agent (recurring entity, 50% continuity) never fires
# E-B: true contradiction (entity re-appears in alien context) fires
# E-C: context switch fires once at the transition, then stops (no latching)
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

# EXPERIMENT (reversible in one commit). See test_absorption_degenerate.sh, which
# has carried this guard alone since before this change:
#   "Stub-backed HTTP tests hang on Windows/MSYS2 due to signal propagation and
#    process cleanup issues. Skip entirely — Linux CI validates the behavior."
# That diagnosis was made once and applied to one file out of 29 that launch the
# stub. build-windows has since stalled four times inside "CLI tests — shell
# suites": the step sits in_progress ~47 minutes, the runner is killed
# service-side, and the log archive 404s. timeout-minutes failed to fire TWICE,
# so the runner cannot enforce its own step timeout either — the cause cannot be
# read, only excluded.
#
# Read the next Windows run as the result:
#   green   -> stub-backed suites are the cause; narrow from here
#   stalls  -> they are exonerated and the cause is elsewhere in the phase
#
# Coverage is not lost: build-linux and Build & Test run every one of these in
# full, and agent-governance semantics are platform-neutral.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        echo "  test_entity_window.sh: SKIPPED (stub-backed; excluded on Windows pending stall bisect)"
        exit 0 ;;
esac
if [ -n "${WINDIR:-}" ]; then
    echo "  test_entity_window.sh: SKIPPED (stub-backed; excluded on Windows pending stall bisect)"
    exit 0
fi


if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/entwin-$$"

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
export FAKE_KEY_ENTWIN_TEST="fake-key-entity-window-test"

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

write_govern() {  # $1=dir
    cat > "$1/govern.json" << GOVEOF
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
            "context_growth": false, "instruction_recall": false, "plan_drift": false,
            "entity_consistency": true, "instruction_conflict": false,
            "persona_fingerprint": false, "tool_chain_integrity": false,
            "claim_result_reconciliation": false, "prompt_compliance": false,
            "response_repetition": false
        } },
    "agents": {
        "worker": {
            "provider": "gemini", "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": "FAKE_KEY_ENTWIN_TEST", "max_tokens": 100, "max_turns": 30
        }
    }
}
GOVEOF
}

write_script() {  # $1=dir $2=send_count
    cat > "$1/test.naab" << NAABEOF
use agent
main {
    let h = agent.create("worker")
    for i in 0..$2 {
        let r = agent.send(h, "continue the gadgetron work")
    }
    print("DONE")
}
NAABEOF
}

# Number of CDD turns where entity_consistency fired (SEMANTIC_TURN is only
# emitted when S10/S11 are enabled, so count CDD_TURN signals_detail instead)
last_entity_count() {  # $1=telemetry file
    grep '"event_type":"CDD_TURN"' "$1" 2>/dev/null \
        | grep -c '"signals_detail":"[^"]*entity_consistency'
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|     entity_consistency (S15) — windowed sighting comparison  |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

# ============================================================
# E-A: recurring entity with 50% vocabulary continuity — the exact
# scenario that fired forever pre-fix — must never fire now.
# Response i: gadgetron + 3 words carried from turn i-1 + 3 new words.
# ============================================================
echo -e "${CYAN}--- E-A: coherent evolving agent never fires ---${NC}"
W=(ablefox bakerhat candlewick doverplum easelbird fangrove gimletree hovercane
   ironmoss jasperkey kelpstone lumenpath mirthwood nectarbay orchidfen pumicejar
   quartzvale rowanberry saddlecliff timberlark umberfield violetmarsh wickergate
   xenonbrook yarrowdell zephyrmill ambergrove)

WDIR="$TEST_TMP/ea"; mkdir -p "$WDIR"
{
    echo '{"responses": ['
    for i in $(seq 0 7); do
        [ $i -gt 0 ] && echo ','
        new="${W[$((3*i))]} ${W[$((3*i+1))]} ${W[$((3*i+2))]}"
        if [ $i -gt 0 ]; then
            old="${W[$((3*(i-1)))]} ${W[$((3*(i-1)+1))]} ${W[$((3*(i-1)+2))]}"
        else
            old=""
        fi
        printf '  {"content": "gadgetron integrates %s %s", "output_tokens": 20}' "$old" "$new"
    done
    echo ''
    echo ']}'
} > "$WDIR/fixture.json"
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "EA-00" "stub failed to start"; exit 1; }
write_govern "$WDIR"
sign_govern "$WDIR"
write_script "$WDIR" 8
OUTPUT=$(cd "$WDIR" && timeout 90s "$NAAB" test.naab 2>&1) || true
stop_stub

echo "$OUTPUT" | grep -q "DONE" && pass "EA-01" "8 sends complete" \
    || fail "EA-01" "sends did not complete" "$(echo "$OUTPUT" | head -3)"
COUNT=$(last_entity_count "$WDIR/telemetry.jsonl")
if [ "$COUNT" = "0" ]; then
    pass "EA-02" "S15 never fires on gradual vocabulary evolution (count=0)"
else
    fail "EA-02" "S15 fired on a coherent evolving agent" "entity_consistency_count=$COUNT"
fi

# ============================================================
# E-B: true contradiction — entity re-appears in a completely alien
# context within the window. Must fire.
# ============================================================
echo -e "${CYAN}--- E-B: alien context fires ---${NC}"
WDIR="$TEST_TMP/eb"; mkdir -p "$WDIR"
cat > "$WDIR/fixture.json" << 'EOF'
{"responses": [
  {"content": "gadgetron integrates ablefox bakerhat candlewick doverplum easelbird", "output_tokens": 20},
  {"content": "gadgetron devours zebrafish quandary monsoon parchment lighthouse", "output_tokens": 20}
]}
EOF
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "EB-00" "stub failed to start"; exit 1; }
write_govern "$WDIR"
sign_govern "$WDIR"
write_script "$WDIR" 2
OUTPUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1) || true
stop_stub

COUNT=$(last_entity_count "$WDIR/telemetry.jsonl")
if [ -n "$COUNT" ] && [ "$COUNT" -ge 1 ] 2>/dev/null; then
    pass "EB-01" "S15 fires when the entity re-appears in an alien context (count=$COUNT)"
else
    fail "EB-01" "S15 missed a real contradiction" "entity_consistency_count=${COUNT:-missing}"
fi

# ============================================================
# E-C: permanent context switch — fires at the transition turn, then the
# new context becomes a recent sighting and firing STOPS (no latching).
# 4 turns in context A, then 4 turns in context B.
# ============================================================
echo -e "${CYAN}--- E-C: context switch fires once, then stops ---${NC}"
WDIR="$TEST_TMP/ec"; mkdir -p "$WDIR"
{
    echo '{"responses": ['
    for i in $(seq 0 3); do
        [ $i -gt 0 ] && echo ','
        printf '  {"content": "gadgetron integrates ablefox bakerhat candlewick doverplum easelbird", "output_tokens": 20}'
    done
    for i in $(seq 0 3); do
        echo ','
        printf '  {"content": "gadgetron devours zebrafish quandary monsoon parchment lighthouse", "output_tokens": 20}'
    done
    echo ''
    echo ']}'
} > "$WDIR/fixture.json"
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "EC-00" "stub failed to start"; exit 1; }
write_govern "$WDIR"
sign_govern "$WDIR"
write_script "$WDIR" 8
OUTPUT=$(cd "$WDIR" && timeout 90s "$NAAB" test.naab 2>&1) || true
stop_stub

COUNT=$(last_entity_count "$WDIR/telemetry.jsonl")
if [ "$COUNT" = "1" ]; then
    pass "EC-01" "S15 fires exactly once at the context transition (count=1)"
else
    fail "EC-01" "S15 latched or missed the transition" "entity_consistency_count=${COUNT:-missing} (expected 1)"
fi

echo ""
echo "================================================"
echo "  PASS: $PASS_COUNT  FAIL: $FAIL_COUNT  SKIP: $SKIP_COUNT"
echo "================================================"
[ -n "$FAILURES" ] && echo -e "Failures:$FAILURES"

exit "$FAIL_COUNT"
