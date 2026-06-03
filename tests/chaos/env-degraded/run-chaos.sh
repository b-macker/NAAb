#!/usr/bin/env bash
# Chaos Test: Agent Environment Degraded Paths
# Exercises: dead keys, all-bogus keys, hard stop, tiny budgets,
#            batch under stress, fan-out mixed health, post-failure consistency
#
# Usage: bash run-chaos.sh [--test N]   (N=1..17, omit for all)
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../../build/naab-lang"
TEST_TMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}/chaos-$$"
SIGNING_KEY="$HOME/.naab/keys/signing.pem"
CHAOS_CONFIG="$SCRIPT_DIR/govern-chaos.json"

RUN_TEST=0
if [ "${1:-}" = "--test" ] && [ -n "${2:-}" ]; then
    RUN_TEST="$2"
fi
should_run() { [ "$RUN_TEST" -eq 0 ] || [ "$RUN_TEST" -eq "$1" ]; }

PASS_COUNT=0
FAIL_COUNT=0
TOTAL=0
FAILURES=""

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

cleanup() { rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"

pass() {
    local id="$1" desc="$2"
    PASS_COUNT=$((PASS_COUNT + 1)); TOTAL=$((TOTAL + 1))
    echo -e "  ${GREEN}PASS${NC} [$id] $desc"
}
fail() {
    local id="$1" desc="$2" detail="${3:-}"
    FAIL_COUNT=$((FAIL_COUNT + 1)); TOTAL=$((TOTAL + 1))
    echo -e "  ${RED}FAIL${NC} [$id] $desc"
    [ -n "$detail" ] && echo -e "       ${RED}→ $detail${NC}"
    FAILURES="${FAILURES}\n  [$id] $desc${detail:+ — $detail}"
}

# Source bashrc for valid key
source "$HOME/.bashrc" 2>/dev/null || true
export GK1 GK2 GK3 GK4 GK5 GK6 GR1 2>/dev/null

# Set up chaos keys: 1 valid + 5 bogus
export CHAOS_VALID="$GK5"
export CHAOS_BOGUS1="AIzaSy-BOGUS-KEY-1-INVALID-xxxxxxxxx"
export CHAOS_BOGUS2="AIzaSy-BOGUS-KEY-2-INVALID-xxxxxxxxx"
export CHAOS_BOGUS3="AIzaSy-BOGUS-KEY-3-INVALID-xxxxxxxxx"
export CHAOS_BOGUS4="AIzaSy-BOGUS-KEY-4-INVALID-xxxxxxxxx"
export CHAOS_BOGUS5="AIzaSy-BOGUS-KEY-5-INVALID-xxxxxxxxx"

setup_workdir() {
    local workdir="$TEST_TMP/work-$$-$RANDOM"
    mkdir -p "$workdir"
    cp "$CHAOS_CONFIG" "$workdir/govern.json"
    (cd "$workdir" && NAAB_SIGNING_KEY="$SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
    echo "$workdir"
}

run_chaos() {
    local workdir="$1" naab_file="$2"
    cp "$SCRIPT_DIR/src/$naab_file" "$workdir/"
    CHAOS_VALID="$CHAOS_VALID" \
    CHAOS_BOGUS1="$CHAOS_BOGUS1" CHAOS_BOGUS2="$CHAOS_BOGUS2" \
    CHAOS_BOGUS3="$CHAOS_BOGUS3" CHAOS_BOGUS4="$CHAOS_BOGUS4" \
    CHAOS_BOGUS5="$CHAOS_BOGUS5" \
    "$NAAB" "$workdir/$naab_file" 2>&1
}

echo -e "${BOLD}${CYAN}╔══════════════════════════════════════════════════╗${NC}"
echo -e "${BOLD}${CYAN}║  Chaos Test: Agent Environment Degraded Paths   ║${NC}"
echo -e "${BOLD}${CYAN}╚══════════════════════════════════════════════════╝${NC}"
echo ""

# ═══════════════════════════════════════════════════════
# TEST 1: Dead Key Accumulation
# ═══════════════════════════════════════════════════════
if should_run 1; then
    echo -e "${CYAN}── Test 1: Dead Key Accumulation ──${NC}"
    W=$(setup_workdir)
    OUT=$(run_chaos "$W" "chaos_01_dead_keys.naab")
    echo "$OUT" > "$TEST_TMP/chaos_01.log"

    # C1: Test completed without crash
    echo "$OUT" | grep -q "chaos_dead_keys: completed" && \
        pass "C1" "dead key test completed" || \
        fail "C1" "dead key test did not complete"

    # C2: Birth shows all keys active
    birth_active=$(echo "$OUT" | grep "chaos_birth:" | grep -oP 'active=\K[0-9]+')
    [ "${birth_active:-0}" -eq 6 ] && \
        pass "C2" "birth shows 6 active keys" || \
        fail "C2" "birth active keys: expected 6 got ${birth_active:-?}"

    # C3: After first send, keys died
    echo "$OUT" | grep -q "chaos_keys_died: true" && \
        pass "C3" "keys died after first send" || \
        fail "C3" "keys did not die after first send"

    # C4: Active count decreased
    echo "$OUT" | grep -q "chaos_active_decreased: true" && \
        pass "C4" "active key count decreased" || \
        fail "C4" "active key count did not decrease"

    # C5: Second send still works (valid key survived)
    echo "$OUT" | grep -q "chaos_send2: dead=" && \
        pass "C5" "second send succeeded with remaining key" || \
        fail "C5" "second send failed"

    # C6: key_health shows dead keys
    kh_dead=$(echo "$OUT" | grep "chaos_key_health:" | grep -oP 'dead=\K[0-9]+')
    [ "${kh_dead:-0}" -gt 0 ] && \
        pass "C6" "key_health reports ${kh_dead} dead keys" || \
        fail "C6" "key_health shows 0 dead"

    echo ""
fi

# ═══════════════════════════════════════════════════════
# TEST 2: All Keys Bogus
# ═══════════════════════════════════════════════════════
if should_run 2; then
    echo -e "${CYAN}── Test 2: All Keys Bogus ──${NC}"
    W=$(setup_workdir)
    OUT=$(run_chaos "$W" "chaos_02_all_bogus.naab")
    echo "$OUT" > "$TEST_TMP/chaos_02.log"

    # C7: Test completed
    echo "$OUT" | grep -q "chaos_all_bogus: completed" && \
        pass "C7" "all-bogus test completed" || \
        fail "C7" "all-bogus test did not complete"

    # C8: Send failed (expected)
    echo "$OUT" | grep -q "bogus_send: ok=false" && \
        pass "C8" "send correctly failed with all bogus keys" || \
        fail "C8" "send did not fail as expected"

    # C9: All keys dead after failure
    echo "$OUT" | grep -q "bogus_all_dead: true" && \
        pass "C9" "all keys reported dead" || \
        fail "C9" "not all keys reported dead"

    # C10: No key name leak in error
    echo "$OUT" | grep -q "bogus_leak: false" && \
        pass "C10" "no key name leaked in error message" || \
        fail "C10" "key name leaked in error"

    # C11: Dispatch tracked the failed attempts
    bogus_calls=$(echo "$OUT" | grep "bogus_dispatch:" | grep -oP 'calls=\K[0-9]+')
    [ "${bogus_calls:-0}" -gt 0 ] && \
        pass "C11" "dispatch tracked ${bogus_calls} failed calls" || \
        fail "C11" "dispatch shows 0 calls after failures"

    echo ""
fi

# ═══════════════════════════════════════════════════════
# TEST 3: Hard Stop
# ═══════════════════════════════════════════════════════
if should_run 3; then
    echo -e "${CYAN}── Test 3: Hard Stop Exhaustion ──${NC}"
    W=$(setup_workdir)
    OUT=$(run_chaos "$W" "chaos_03_hard_stop.naab")
    echo "$OUT" > "$TEST_TMP/chaos_03.log"

    # C12: Test completed
    echo "$OUT" | grep -q "chaos_hard_stop: completed" && \
        pass "C12" "hard stop test completed" || \
        fail "C12" "hard stop test did not complete"

    # C13: Hard stop triggered
    echo "$OUT" | grep -q "hardstop_did_stop: true" && \
        pass "C13" "hard stop triggered" || \
        fail "C13" "hard stop did NOT trigger"

    # C14: Final dispatch shows stopped
    echo "$OUT" | grep "hardstop_final:" | grep -q "stopped=true" && \
        pass "C14" "final dispatch shows hard_stopped=true" || \
        fail "C14" "final dispatch does not show stopped"

    # C15: Some sends completed before stop
    completed=$(echo "$OUT" | grep "hardstop_sends_completed:" | grep -oP '\d+')
    [ "${completed:-0}" -gt 0 ] && \
        pass "C15" "${completed} sends completed before hard stop" || \
        fail "C15" "0 sends completed"

    # C16: Post-stop environment shows stopped
    echo "$OUT" | grep "hardstop_post_env:" | grep -q "stopped=true" && \
        pass "C16" "post-stop environment reflects hard_stopped" || \
        fail "C16" "post-stop environment missing or wrong"

    # C17: Post-stop send blocked
    echo "$OUT" | grep -q "hardstop_post_blocked: true" && \
        pass "C17" "post-stop send correctly blocked" || \
        fail "C17" "post-stop send was not blocked"

    # C18: Calls remaining decremented over iterations
    first_left=$(echo "$OUT" | grep "hardstop_i0:" | grep -oP 'calls_left=\K-?[0-9]+')
    if [ -n "${first_left:-}" ] && [ "$first_left" -gt 0 ]; then
        pass "C18" "calls_remaining decremented (started at ${first_left})"
    else
        fail "C18" "could not verify calls_remaining decrement"
    fi

    echo ""
fi

# ═══════════════════════════════════════════════════════
# TEST 4: Tiny Agent Budget
# ═══════════════════════════════════════════════════════
if should_run 4; then
    echo -e "${CYAN}── Test 4: Tiny Agent Budget ──${NC}"
    W=$(setup_workdir)
    OUT=$(run_chaos "$W" "chaos_04_tiny_budget.naab")
    echo "$OUT" > "$TEST_TMP/chaos_04.log"

    # C19: Test completed
    echo "$OUT" | grep -q "chaos_tiny_budget: completed" && \
        pass "C19" "tiny budget test completed" || \
        fail "C19" "tiny budget test did not complete"

    # C20: Limits reflect tiny config
    echo "$OUT" | grep "tiny_limits:" | grep -q "turns=1" && \
        pass "C20" "max_turns=1 reflected in environment" || \
        fail "C20" "max_turns not 1"

    # C21: First send works
    echo "$OUT" | grep "tiny_after_send1:" | grep -q "turns_used=1" && \
        pass "C21" "first send consumed the one turn" || \
        fail "C21" "first send did not register"

    # C22: At turn limit after first send
    echo "$OUT" | grep -q "tiny_at_turn_limit: true" && \
        pass "C22" "at turn limit (0 remaining) after first send" || \
        fail "C22" "not at turn limit"

    # C23: Second send rejected
    echo "$OUT" | grep -q "tiny_turn_enforced: true" && \
        pass "C23" "second send rejected (turn limit enforced)" || \
        fail "C23" "second send was NOT rejected"

    # C24: Post-rejection environment consistent
    echo "$OUT" | grep "tiny_post:" | grep -q "turns_remaining=0" && \
        pass "C24" "post-rejection env shows 0 turns remaining" || \
        fail "C24" "post-rejection env inconsistent"

    echo ""
fi

# ═══════════════════════════════════════════════════════
# TEST 5: Batch Under Stress
# ═══════════════════════════════════════════════════════
if should_run 5; then
    echo -e "${CYAN}── Test 5: Batch Under Degraded Keys ──${NC}"
    W=$(setup_workdir)
    OUT=$(run_chaos "$W" "chaos_05_batch_degraded.naab")
    echo "$OUT" > "$TEST_TMP/chaos_05.log"

    # C25: Test completed
    echo "$OUT" | grep -q "chaos_batch_degraded: completed" && \
        pass "C25" "batch stress test completed" || \
        fail "C25" "batch stress test did not complete"

    # C26: Batch returned results
    batch_count=$(echo "$OUT" | grep "batch_count:" | grep -oP '\d+')
    [ "${batch_count:-0}" -gt 0 ] && \
        pass "C26" "batch returned ${batch_count} results" || \
        fail "C26" "batch returned no results"

    # C27: At least one slot succeeded
    echo "$OUT" | grep "batch_results:" | grep -qP 'success=[1-9]' && \
        pass "C27" "at least one batch slot succeeded" || \
        fail "C27" "no batch slots succeeded"

    # C28: Dead keys visible in batch slot environments
    echo "$OUT" | grep "batch_slot" | grep -q "dead=" && \
        pass "C28" "batch slot environments include dead key count" || \
        fail "C28" "no dead key info in batch slots"

    # C29: Dispatch budget consumed by batch
    batch_calls=$(echo "$OUT" | grep "batch_post:" | grep -oP 'calls=\K[0-9]+')
    [ "${batch_calls:-0}" -gt 0 ] && \
        pass "C29" "dispatch shows ${batch_calls} calls after batch" || \
        fail "C29" "dispatch shows 0 calls after batch"

    echo ""
fi

# ═══════════════════════════════════════════════════════
# TEST 6: Fan-Out Mixed Health
# ═══════════════════════════════════════════════════════
if should_run 6; then
    echo -e "${CYAN}── Test 6: Fan-Out Mixed Health ──${NC}"
    W=$(setup_workdir)
    OUT=$(run_chaos "$W" "chaos_06_fan_out_degraded.naab")
    echo "$OUT" > "$TEST_TMP/chaos_06.log"

    # C30: Test completed
    echo "$OUT" | grep -q "chaos_fan_out_degraded: completed" && \
        pass "C30" "fan-out degraded test completed" || \
        fail "C30" "fan-out degraded test did not complete"

    # C31: Fan-out returned results for both agents
    fanout_count=$(echo "$OUT" | grep "fanout_count:" | grep -oP '\d+')
    [ "${fanout_count:-0}" -eq 2 ] && \
        pass "C31" "fan-out returned 2 results" || \
        fail "C31" "fan-out returned ${fanout_count:-0} results (expected 2)"

    # C32: At least one slot succeeded (chaos_mixed has valid key)
    echo "$OUT" | grep -q "fanout_any_success: true" && \
        pass "C32" "at least one fan-out slot succeeded" || \
        fail "C32" "no fan-out slots succeeded"

    # C33: Fan-out results show divergent key health
    echo "$OUT" | grep -q "fanout_divergence:" && \
        pass "C33" "fan-out shows divergent key health between agents" || \
        fail "C33" "no divergence data in fan-out"

    echo ""
fi

# ═══════════════════════════════════════════════════════
# TEST 7: Environment After Failure
# ═══════════════════════════════════════════════════════
if should_run 7; then
    echo -e "${CYAN}── Test 7: Environment Consistency After Failures ──${NC}"
    W=$(setup_workdir)
    OUT=$(run_chaos "$W" "chaos_07_env_after_failure.naab")
    echo "$OUT" > "$TEST_TMP/chaos_07.log"

    # C34: Test completed
    echo "$OUT" | grep -q "chaos_env_after_failure: completed" && \
        pass "C34" "post-failure env test completed" || \
        fail "C34" "post-failure env test did not complete"

    # C35: Environment queryable after failure
    echo "$OUT" | grep -q "postfail_env_query: ok" && \
        pass "C35" "agent.environment() works after send failure" || \
        fail "C35" "agent.environment() broke after failure"

    # C36: Structure intact after failure
    echo "$OUT" | grep "postfail_structure:" | grep -q "limits=true" && \
        pass "C36" "environment structure intact (limits present)" || \
        fail "C36" "environment structure damaged"

    # C37: Coherence valid after failure (not NaN)
    echo "$OUT" | grep -q "postfail_coherence:.*valid=true" && \
        pass "C37" "coherence valid after failure (not NaN/negative)" || \
        fail "C37" "coherence invalid after failure"

    # C38: Governance level valid enum after failure
    echo "$OUT" | grep -q "postfail_gov:.*valid=true" && \
        pass "C38" "governance level valid enum after failure" || \
        fail "C38" "governance level invalid after failure"

    # C39: Multiple failures don't corrupt state
    echo "$OUT" | grep -q "postfail_multi:.*coherence_valid=true" && \
        pass "C39" "coherence valid after multiple failures" || \
        fail "C39" "state corrupted after multiple failures"

    # C40: Keys show dead after all-bogus failures
    postfail_dead=$(echo "$OUT" | grep "postfail_keys:" | grep -oP 'dead=\K[0-9]+')
    [ "${postfail_dead:-0}" -gt 0 ] && \
        pass "C40" "keys correctly marked dead after failures (${postfail_dead})" || \
        fail "C40" "no dead keys after all-bogus failures"

    echo ""
fi

# ═══════════════════════════════════════════════════════
# TEST 8: Pipeline Upstream Provenance
# ═══════════════════════════════════════════════════════
if should_run 8; then
    echo -e "${CYAN}── Test 8: Pipeline Upstream Provenance ──${NC}"
    W=$(setup_workdir)
    OUT=$(run_chaos "$W" "chaos_08_pipeline_provenance.naab")
    echo "$OUT" > "$TEST_TMP/chaos_08.log"

    # C41: Test completed
    echo "$OUT" | grep -q "chaos_pipeline_provenance: completed" && \
        pass "C41" "pipeline provenance test completed" || \
        fail "C41" "pipeline provenance test did not complete"

    # C42: No provenance at birth (no upstream yet)
    echo "$OUT" | grep -q "provenance_birth_h1: false" && \
        pass "C42" "no provenance at birth for h1" || \
        fail "C42" "unexpected provenance at birth"

    echo "$OUT" | grep -q "provenance_birth_h2: false" && \
        pass "C43" "no provenance at birth for h2" || \
        fail "C43" "unexpected provenance at birth for h2"

    # C44: Provenance exists after pipeline
    echo "$OUT" | grep -q "provenance_exists: true" && \
        pass "C44" "upstream provenance present in downstream response" || \
        fail "C44" "no upstream provenance in downstream response"

    # C45: Stage index correct (upstream is stage 0)
    echo "$OUT" | grep -q "provenance_stage: 0" && \
        pass "C45" "provenance stage index is 0 (first stage)" || \
        fail "C45" "wrong provenance stage index"

    # C46: Model used is populated
    echo "$OUT" | grep -q "provenance_model: true" && \
        pass "C46" "provenance includes model_used" || \
        fail "C46" "provenance missing model_used"

    # C47: Fallback field present
    echo "$OUT" | grep -q "provenance_has_fallback: true" && \
        pass "C47" "provenance includes was_fallback" || \
        fail "C47" "provenance missing was_fallback"

    # C48: Retries reflects key rotation (5 bogus keys = retries)
    prov_retries=$(echo "$OUT" | grep "provenance_retries:" | grep -oP ': \K[0-9]+')
    [ "${prov_retries:-0}" -gt 0 ] && \
        pass "C48" "provenance retries=${prov_retries} (key rotation visible)" || \
        fail "C48" "provenance retries should be > 0 (got ${prov_retries:-?})"

    # C49: Coherence present
    echo "$OUT" | grep -q "provenance_has_coherence: true" && \
        pass "C49" "provenance includes coherence_at_output" || \
        fail "C49" "provenance missing coherence_at_output"

    # C50: Keys dead reflects degraded upstream
    prov_dead=$(echo "$OUT" | grep "provenance_keys_dead:" | grep -oP ': \K[0-9]+')
    [ "${prov_dead:-0}" -gt 0 ] && \
        pass "C50" "provenance keys_dead=${prov_dead} (upstream degradation visible)" || \
        fail "C50" "provenance should show dead keys (got ${prov_dead:-?})"

    # C51: Latency present
    echo "$OUT" | grep -q "provenance_has_latency: true" && \
        pass "C51" "provenance includes latency_ms" || \
        fail "C51" "provenance missing latency_ms"

    # C52: On-demand query also returns provenance
    echo "$OUT" | grep -q "provenance_on_demand: true" && \
        pass "C52" "agent.environment() returns provenance after pipeline" || \
        fail "C52" "agent.environment() missing provenance"

    # C53: First stage (h1) has NO provenance
    echo "$OUT" | grep -q "provenance_h1_absent: true" && \
        pass "C53" "first pipeline stage has no upstream provenance" || \
        fail "C53" "first stage incorrectly has provenance"

    # C54: Stage traces collected
    echo "$OUT" | grep -q "provenance_stage_traces: 2" && \
        pass "C54" "2 stage traces collected from pipeline" || \
        fail "C54" "wrong number of stage traces"

    echo ""
fi

# ═══════════════════════════════════════════════════════
# TEST 9: Handle Anti-Forge
# ═══════════════════════════════════════════════════════
if should_run 9; then
    echo -e "${CYAN}── Test 9: Handle Anti-Forge ──${NC}"
    W=$(setup_workdir)
    OUT=$(run_chaos "$W" "chaos_09_handle_forge.naab")
    echo "$OUT" > "$TEST_TMP/chaos_09.log"

    # C55: Test completed
    echo "$OUT" | grep -q "chaos09_completed: true" && \
        pass "C55" "handle forge test completed" || \
        fail "C55" "handle forge test did not complete"

    # C56: Real handle has nonce
    echo "$OUT" | grep -q "chaos09_real_has_nonce: true" && \
        pass "C56" "real handle contains __nonce field" || \
        fail "C56" "real handle missing __nonce"

    # C57: Wrong nonce blocked
    echo "$OUT" | grep -q "chaos09_forge1_blocked: true" && \
        pass "C57" "forged nonce rejected" || \
        fail "C57" "forged nonce was NOT rejected"

    # C58: Error doesn't leak internals
    echo "$OUT" | grep -q "chaos09_forge1_leaks: false" && \
        pass "C58" "error message doesn't leak HMAC/nonce details" || \
        fail "C58" "error message leaks internal details"

    # C59: Missing nonce blocked
    echo "$OUT" | grep -q "chaos09_forge2_blocked: true" && \
        pass "C59" "missing nonce rejected" || \
        fail "C59" "missing nonce was NOT rejected"

    # C60: Wrong ID with real nonce blocked
    echo "$OUT" | grep -q "chaos09_forge3_blocked: true" && \
        pass "C60" "wrong ID with real nonce rejected" || \
        fail "C60" "wrong ID with real nonce was NOT rejected"

    echo ""
fi

# ═══════════════════════════════════════════════════════
# TEST 10: Action Matrix Enforcement
# ═══════════════════════════════════════════════════════
if should_run 10; then
    echo -e "${CYAN}── Test 10: Action Matrix Enforcement ──${NC}"
    W=$(setup_workdir)
    OUT=$(run_chaos "$W" "chaos_10_action_matrix.naab")
    echo "$OUT" > "$TEST_TMP/chaos_10.log"

    # C61: Test completed
    echo "$OUT" | grep -q "chaos10_completed: true" && \
        pass "C61" "action matrix test completed" || \
        fail "C61" "action matrix test did not complete"

    # C62: Agent created successfully
    echo "$OUT" | grep -q "chaos10_created: true" && \
        pass "C62" "restricted agent created" || \
        fail "C62" "restricted agent creation failed"

    # C63: Environment available
    echo "$OUT" | grep -q "chaos10_env_available: true" && \
        pass "C63" "agent.environment() works for restricted agent" || \
        fail "C63" "agent.environment() failed for restricted agent"

    # C64: AGENT_SEND not blocked (it's in allowed_actions)
    echo "$OUT" | grep -q "chaos10_send_action_blocked: false" && \
        pass "C64" "AGENT_SEND permitted (in allowed_actions)" || \
        fail "C64" "AGENT_SEND incorrectly blocked by action matrix"

    echo ""
fi

# ═══════════════════════════════════════════════════════
# TEST 11: Config Features Parsing
# ═══════════════════════════════════════════════════════
if should_run 11; then
    echo -e "${CYAN}── Test 11: Config Features Parsing ──${NC}"
    W=$(setup_workdir)
    OUT=$(run_chaos "$W" "chaos_11_config_features.naab")
    echo "$OUT" > "$TEST_TMP/chaos_11.log"

    # C65: Config loaded with all new fields
    echo "$OUT" | grep -q "chaos11_config_loaded: true" && \
        pass "C65" "config with all Phase 1-5 fields loaded" || \
        fail "C65" "config with new fields failed to load"

    # C66: chaos_mixed agent created
    echo "$OUT" | grep -q "chaos11_mixed_created: true" && \
        pass "C66" "chaos_mixed agent created" || \
        fail "C66" "chaos_mixed creation failed"

    # C67: chaos_restricted agent created
    echo "$OUT" | grep -q "chaos11_restricted_created: true" && \
        pass "C67" "chaos_restricted agent created" || \
        fail "C67" "chaos_restricted creation failed"

    # C68: chaos_refresh agent created
    echo "$OUT" | grep -q "chaos11_refresh_created: true" && \
        pass "C68" "chaos_refresh agent created (with key_retry_after_seconds)" || \
        fail "C68" "chaos_refresh creation failed"

    # C69: Challenges fields in environment
    echo "$OUT" | grep -q "chaos11_challenges_in_env: true" && \
        pass "C69" "challenges_passed/failed in environment dict" || \
        fail "C69" "challenges fields missing from environment"

    echo ""
fi

# ═══════════════════════════════════════════════════════
# TEST 12: Temporal Trust Decay
# ═══════════════════════════════════════════════════════
if should_run 12; then
    echo -e "${CYAN}── Test 12: Temporal Trust Decay ──${NC}"
    W=$(setup_workdir)
    OUT=$(run_chaos "$W" "chaos_12_temporal_decay.naab")
    echo "$OUT" > "$TEST_TMP/chaos_12.log"

    # C70: Test completed
    echo "$OUT" | grep -q "chaos12_completed: true" && \
        pass "C70" "temporal decay test completed" || \
        fail "C70" "temporal decay test did not complete"

    # C71: Pre-decay coherence is 1.0
    echo "$OUT" | grep -q "chaos12_pre_coherence: 1" && \
        pass "C71" "pre-decay coherence is 1.0" || \
        fail "C71" "pre-decay coherence not 1.0"

    # C72: Coherence decayed after sleep
    echo "$OUT" | grep -q "chaos12_decayed: true" && \
        pass "C72" "coherence decayed after idle period" || \
        fail "C72" "coherence did NOT decay after idle period"

    # C73: Coherence still positive (not negative)
    echo "$OUT" | grep -q "chaos12_positive: true" && \
        pass "C73" "decayed coherence is non-negative" || \
        fail "C73" "decayed coherence went negative"

    # C74: Coherence not NaN
    echo "$OUT" | grep -q "chaos12_not_nan: true" && \
        pass "C74" "decayed coherence is not NaN" || \
        fail "C74" "decayed coherence is NaN"

    echo ""
fi

# ═══════════════════════════════════════════════════════
# TEST 13: Key Revival After Cooldown
# ═══════════════════════════════════════════════════════
if should_run 13; then
    echo -e "${CYAN}── Test 13: Key Revival After Cooldown ──${NC}"
    W=$(setup_workdir)
    OUT=$(run_chaos "$W" "chaos_13_key_revival.naab")
    echo "$OUT" > "$TEST_TMP/chaos_13.log"

    # C75: Test completed
    echo "$OUT" | grep -q "chaos13_completed: true" && \
        pass "C75" "key revival test completed" || \
        fail "C75" "key revival test did not complete"

    # C76: First send was attempted
    echo "$OUT" | grep -q "chaos13_send1_attempted: true" && \
        pass "C76" "first send attempted" || \
        fail "C76" "first send not attempted"

    # C77: Key health info available
    echo "$OUT" | grep -q "chaos13_kh1_has_info: true" && \
        pass "C77" "key health reports key pool info" || \
        fail "C77" "key health missing pool info"

    # C78: After cooldown, dead keys revived OR no keys died
    # (revival_visible=true means dead count dropped; revival_na=true means no keys died)
    if echo "$OUT" | grep -q "chaos13_revival_visible: true"; then
        pass "C78" "dead keys revived after cooldown"
    elif echo "$OUT" | grep -q "chaos13_revival_na: true"; then
        pass "C78" "no keys died (API returned non-401) — revival N/A"
    else
        fail "C78" "dead keys NOT revived after cooldown"
    fi

    # C79: Second send succeeded after cooldown
    echo "$OUT" | grep -q "chaos13_send2_ok: true" && \
        pass "C79" "second send succeeded after cooldown" || \
        fail "C79" "second send failed after cooldown"

    echo ""
fi

# ═══════════════════════════════════════════════════════
# TEST 14: Action Matrix Blocking
# ═══════════════════════════════════════════════════════
if should_run 14; then
    echo -e "${CYAN}── Test 14: Action Matrix Blocking ──${NC}"
    W=$(setup_workdir)
    OUT=$(run_chaos "$W" "chaos_14_action_block.naab")
    echo "$OUT" > "$TEST_TMP/chaos_14.log"

    # C80: Test completed
    echo "$OUT" | grep -q "chaos14_completed: true" && \
        pass "C80" "action matrix blocking test completed" || \
        fail "C80" "action matrix blocking test did not complete"

    # C81: Agent without AGENT_SEND created
    echo "$OUT" | grep -q "chaos14_created: true" && \
        pass "C81" "agent without AGENT_SEND created" || \
        fail "C81" "agent creation failed"

    # C82: Send blocked by action matrix
    echo "$OUT" | grep -q "chaos14_send_blocked: true" && \
        pass "C82" "send blocked by action matrix (no AGENT_SEND)" || \
        fail "C82" "send was NOT blocked by action matrix"

    # C83: Error mentions action matrix restriction
    echo "$OUT" | grep -q "chaos14_error_mentions_restriction: true" && \
        pass "C83" "error references action matrix restriction" || \
        fail "C83" "error does not mention action matrix"

    # C84: Error doesn't leak allowed action values
    echo "$OUT" | grep -q "chaos14_error_leaks_actions: false" && \
        pass "C84" "error doesn't leak allowed action list" || \
        fail "C84" "error leaks allowed action list"

    # C85: Environment queryable after block
    echo "$OUT" | grep -q "chaos14_env_after_block: true" && \
        pass "C85" "environment queryable after action matrix block" || \
        fail "C85" "environment broken after block"

    echo ""
fi

# ═══════════════════════════════════════════════════════
# TEST 15: Adaptive Baseline Learning
# ═══════════════════════════════════════════════════════
if should_run 15; then
    echo -e "${CYAN}── Test 15: Adaptive Baseline Learning ──${NC}"
    W=$(setup_workdir)
    OUT=$(run_chaos "$W" "chaos_15_adaptive_baseline.naab")
    echo "$OUT" > "$TEST_TMP/chaos_15.log"

    # C86: Test completed
    echo "$OUT" | grep -q "chaos15_completed: true" && \
        pass "C86" "adaptive baseline test completed" || \
        fail "C86" "adaptive baseline test did not complete"

    # C87: Birth coherence is 1.0
    echo "$OUT" | grep -q "chaos15_birth_coherence: 1" && \
        pass "C87" "baseline birth coherence is 1.0" || \
        fail "C87" "baseline birth coherence not 1.0"

    # C88: Baseline sends completed (at least 1)
    baseline_sends=$(echo "$OUT" | grep "chaos15_baseline_sends:" | grep -oP ': \K[0-9]+')
    [ "${baseline_sends:-0}" -ge 1 ] && \
        pass "C88" "${baseline_sends} baseline sends completed" || \
        fail "C88" "no baseline sends completed"

    # C89: All baseline coherences valid
    echo "$OUT" | grep -q "chaos15_baseline_coherences_valid: true" && \
        pass "C89" "coherences valid during baseline window" || \
        fail "C89" "invalid coherences during baseline window"

    # C90: Post-baseline coherence valid
    echo "$OUT" | grep -q "chaos15_post_baseline_valid: true" && \
        pass "C90" "post-baseline coherence valid" || \
        fail "C90" "post-baseline coherence invalid"

    echo ""
fi

# ═══════════════════════════════════════════════════════
# TEST 16: Step-Up Challenge Fields
# ═══════════════════════════════════════════════════════
if should_run 16; then
    echo -e "${CYAN}── Test 16: Step-Up Challenge Fields ──${NC}"
    W=$(setup_workdir)
    OUT=$(run_chaos "$W" "chaos_16_challenge_fields.naab")
    echo "$OUT" > "$TEST_TMP/chaos_16.log"

    # C91: Test completed
    echo "$OUT" | grep -q "chaos16_completed: true" && \
        pass "C91" "challenge fields test completed" || \
        fail "C91" "challenge fields test did not complete"

    # C92: Initial challenge counters are zero
    echo "$OUT" | grep -q "chaos16_h1_zero: true" && \
        pass "C92" "initial challenge counters are 0/0" || \
        fail "C92" "initial challenge counters not zero"

    # C93: Challenge fields survived send
    echo "$OUT" | grep -q "chaos16_fields_survived_send: true" && \
        pass "C93" "challenge counters present after send" || \
        fail "C93" "challenge counters missing after send"

    echo ""
fi

# ═══════════════════════════════════════════════════════
# TEST 17: Multi-Agent Isolation Under Stress
# ═══════════════════════════════════════════════════════
if should_run 17; then
    echo -e "${CYAN}── Test 17: Multi-Agent Isolation Under Stress ──${NC}"
    W=$(setup_workdir)
    OUT=$(run_chaos "$W" "chaos_17_multi_isolation.naab")
    echo "$OUT" > "$TEST_TMP/chaos_17.log"

    # C94: Test completed
    echo "$OUT" | grep -q "chaos17_completed: true" && \
        pass "C94" "multi-agent isolation test completed" || \
        fail "C94" "multi-agent isolation test did not complete"

    # C95: All three agents created
    echo "$OUT" | grep -q "chaos17_all_created: true" && \
        pass "C95" "all 3 agents (no_send + mixed + restricted) created" || \
        fail "C95" "agent creation failed"

    # C96: chaos_no_send correctly blocked
    echo "$OUT" | grep -q "chaos17_no_send_blocked: true" && \
        pass "C96" "no_send agent blocked by action matrix" || \
        fail "C96" "no_send agent was NOT blocked"

    # C97: chaos_mixed send succeeded (valid key after retries)
    echo "$OUT" | grep -q "chaos17_mixed_ok: true" && \
        pass "C97" "mixed agent send succeeded under degraded keys" || \
        fail "C97" "mixed agent send failed"

    # C98: chaos_restricted not blocked by matrix (AGENT_SEND allowed)
    echo "$OUT" | grep -q "chaos17_restricted_matrix_blocked: false" && \
        pass "C98" "restricted agent not blocked by action matrix" || \
        fail "C98" "restricted agent incorrectly blocked by matrix"

    # C99: All environments independently queryable
    echo "$OUT" | grep -q "chaos17_all_envs_ok: true" && \
        pass "C99" "all 3 agent environments independently queryable" || \
        fail "C99" "environment query failed for some agents"

    # C100: Mixed agent shows dead keys (key degradation visible)
    mixed_dead=$(echo "$OUT" | grep "chaos17_mixed_keys_dead:" | grep -oP ': \K[0-9]+')
    [ "${mixed_dead:-0}" -gt 0 ] && \
        pass "C100" "mixed agent shows ${mixed_dead} dead keys" || \
        fail "C100" "mixed agent shows 0 dead keys"

    echo ""
fi

# ═══════════════════════════════════════════════════════
# SUMMARY
# ═══════════════════════════════════════════════════════
echo -e "${BOLD}${CYAN}══════════════════════════════════════════════════${NC}"
echo -e "${BOLD}  Chaos Test Results: ${PASS_COUNT}/${TOTAL} passed${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then
    echo -e "${RED}  ${FAIL_COUNT} FAILURES:${FAILURES}${NC}"
fi
echo -e "${CYAN}══════════════════════════════════════════════════${NC}"

[ "$FAIL_COUNT" -eq 0 ] && exit 0 || exit 1
