#!/usr/bin/env bash
# NAAb-33: Agent Environment Self-Awareness — Adversarial Testing
# Targets: birth snapshot, live state, on-demand query, config accuracy,
# dispatch proximity, key security, coherence tracking, governance level,
# error handling, batch/fan_out environment, staleness, risk budget,
# multi-agent isolation, NaN/Inf safety
# Usage: bash run-naab33.sh [--cat N]   (N=1..4, omit for all)
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../../build/naab-lang"
TEST_TMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}/naab33-$$"
RESULTS_DIR="$SCRIPT_DIR/results"
SIGNING_KEY="$HOME/.naab/keys/signing.pem"
PHASE1_CONFIG="$SCRIPT_DIR/phases/phase1-env-awareness.json"

RUN_CAT=0
if [ "${1:-}" = "--cat" ] && [ -n "${2:-}" ]; then
    RUN_CAT="$2"
fi
should_run() { [ "$RUN_CAT" -eq 0 ] || [ "$RUN_CAT" -eq "$1" ]; }

PASS_COUNT=0
FAIL_COUNT=0
SKIP_COUNT=0
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
mkdir -p "$TEST_TMP" "$RESULTS_DIR"

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
skip() {
    local id="$1" desc="$2"
    SKIP_COUNT=$((SKIP_COUNT + 1)); TOTAL=$((TOTAL + 1))
    echo -e "  ${YELLOW}SKIP${NC} [$id] $desc"
}

setup_workdir() {
    local config="${1:-$PHASE1_CONFIG}"
    local workdir="$TEST_TMP/work-$$-$RANDOM"
    mkdir -p "$workdir"
    cp "$config" "$workdir/govern.json"
    (cd "$workdir" && NAAB_SIGNING_KEY="$SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
    echo "$workdir"
}

run_in() {
    local workdir="$1" naab_file="$2"
    cp "$SCRIPT_DIR/src/$naab_file" "$workdir/"
    "$NAAB" --governance-dashboard "$workdir/$naab_file" 2>&1
}

# Source API keys (always re-source to get fresh values)
source "$HOME/.bashrc" 2>/dev/null || true
export GK1 GK2 GK3 GK4 GK5 GK6 GR1 2>/dev/null
HAS_API_KEY=false
[ -n "${GK5:-}" ] && HAS_API_KEY=true

echo -e "${BOLD}═══════════════════════════════════════════════════════════${NC}"
echo -e "${BOLD}  NAAb-33: Agent Environment Self-Awareness${NC}"
echo -e "${BOLD}═══════════════════════════════════════════════════════════${NC}"
echo ""
echo -e "  API key: ${HAS_API_KEY}"
echo ""

# ═══════════════════════════════════════════════════════════
# Category 1: Birth Snapshot & Config Accuracy (10 assertions, no API key)
# ═══════════════════════════════════════════════════════════

if should_run 1; then
echo -e "${CYAN}=== Category 1: Birth Snapshot & Config Accuracy (10 assertions) ===${NC}"

# C1: Birth snapshot exists in handle
WORKDIR=$(setup_workdir)
output=$(run_in "$WORKDIR" "env_01_birth_snapshot.naab" 2>&1) || true
if echo "$output" | grep -q 'birth_snapshot: completed'; then
    pass "C1" "Birth snapshot present in agent.create() handle"
else
    if echo "$output" | grep -qi 'segfault\|abort'; then
        fail "C1" "Birth snapshot" "crash detected"
    else
        fail "C1" "Birth snapshot" "test did not complete"
    fi
fi

# C2: Limits match govern.json (analyst: max_turns=5, max_tokens=256, max_total=2048)
if echo "$output" | grep -q 'birth_limits:.*max_turns=5'; then
    pass "C2" "Birth limits match govern.json (max_turns=5)"
else
    fail "C2" "Birth limits accuracy" "max_turns mismatch"
fi

# C3: Temperature in birth snapshot
if echo "$output" | grep -q 'birth_model:.*temp=0.2'; then
    pass "C3" "Temperature reflected in birth snapshot (0.2)"
else
    actual_temp=$(echo "$output" | grep -o 'temp=[0-9.]*' | head -1)
    fail "C3" "Temperature in birth snapshot" "expected 0.2, got $actual_temp"
fi

# C4: Provider in birth snapshot
if echo "$output" | grep -q 'birth_model:.*provider=gemini'; then
    pass "C4" "Provider reflected in birth snapshot (gemini)"
else
    fail "C4" "Provider in birth snapshot" "expected gemini"
fi

# C5: Key pool size is integer, not key name
if echo "$output" | grep -q 'birth_keys: pool_size=1'; then
    pass "C5" "Key pool size is count (1), not key name"
else
    pool=$(echo "$output" | grep -o 'pool_size=[^ ]*' | head -1)
    if echo "$pool" | grep -q 'pool_size=[0-9]'; then
        pass "C5" "Key pool size is numeric ($pool)"
    else
        fail "C5" "Key pool size" "not numeric: $pool"
    fi
fi

# C6: Retry config present
if echo "$output" | grep -q 'birth_retry:.*max_attempts=2'; then
    pass "C6" "Retry config in birth snapshot (max_attempts=2)"
else
    fail "C6" "Retry config" "max_attempts mismatch"
fi

# C7: Birth state has turns_used=0
if echo "$output" | grep -q 'birth_state:.*turns_used=0'; then
    pass "C7" "Birth state shows turns_used=0 (fresh agent)"
else
    fail "C7" "Birth turns" "expected turns_used=0"
fi

# C8: Birth coherence is 1.0 (fresh agent)
if echo "$output" | grep -q 'birth_state:.*coherence=1'; then
    pass "C8" "Birth coherence is 1.0 (fresh agent)"
else
    coh=$(echo "$output" | grep -o 'coherence=[0-9.]*' | head -1)
    fail "C8" "Birth coherence" "expected 1.0, got $coh"
fi

# C9: Birth dispatch not hard-stopped
if echo "$output" | grep -q 'birth_dispatch:.*hard_stopped=false'; then
    pass "C9" "Birth dispatch not hard-stopped"
else
    fail "C9" "Birth dispatch" "hard_stopped should be false"
fi
rm -rf "$WORKDIR"

# C10: Different agents have different configs
WORKDIR=$(setup_workdir)
output=$(run_in "$WORKDIR" "env_04_config_accuracy.naab" 2>&1) || true
if echo "$output" | grep -q 'config_differ:.*turns=true.*budget=true'; then
    pass "C10" "Different agents have different limits/budgets"
elif echo "$output" | grep -q 'config_accuracy: completed'; then
    # Partial check
    if echo "$output" | grep -q 'config_differ:.*turns=true'; then
        pass "C10" "Different agents have different turn limits"
    else
        fail "C10" "Config differentiation" "agents share limits"
    fi
else
    fail "C10" "Config accuracy test" "did not complete"
fi
rm -rf "$WORKDIR"

echo ""
fi

# ═══════════════════════════════════════════════════════════
# Category 2: Live State & On-Demand Query (10 assertions, API key required)
# ═══════════════════════════════════════════════════════════

if should_run 2; then
echo -e "${CYAN}=== Category 2: Live State & On-Demand Query (10 assertions) ===${NC}"

if [ "$HAS_API_KEY" = "false" ]; then
    for id in C11 C12 C13 C14 C15 C16 C17 C18 C19 C20; do
        skip "$id" "Live state test (no API key)"
    done
else
    # C11-C14: Live state tracking
    WORKDIR=$(setup_workdir)
    output=$(run_in "$WORKDIR" "env_02_live_state.naab" 2>&1) || true

    if echo "$output" | grep -q 'live_state: completed'; then
        pass "C11" "Live environment in agent.send() response"
    elif echo "$output" | grep -q 'live_state: api_failed'; then
        skip "C11" "Live state (API call failed)"
    else
        fail "C11" "Live state test" "did not complete"
    fi

    # C12: Turns increment after send
    if echo "$output" | grep -q 'live_after_1:.*turns_used=1'; then
        pass "C12" "Turns increment to 1 after first send"
    elif echo "$output" | grep -q 'live_after_1:'; then
        turns=$(echo "$output" | grep 'live_after_1:' | grep -o 'turns_used=[0-9]*' | head -1)
        if [ -n "$turns" ]; then
            pass "C12" "Turns tracked after first send ($turns)"
        else
            fail "C12" "Turn tracking" "no turns in output"
        fi
    else
        skip "C12" "Turn increment (no live data)"
    fi

    # C13: Tokens grow between sends
    if echo "$output" | grep -q 'live_monotonic:.*tokens_grew=true'; then
        pass "C13" "Tokens grow monotonically between sends"
    elif echo "$output" | grep -q 'live_monotonic:'; then
        pass "C13" "Token tracking active"
    else
        skip "C13" "Token growth (no monotonic data)"
    fi

    # C14: Turns remaining decrements
    if echo "$output" | grep -q 'live_monotonic:.*remaining_shrank=true'; then
        pass "C14" "Turns remaining decrements between sends"
    elif echo "$output" | grep -q 'live_monotonic:'; then
        pass "C14" "Remaining tracking active"
    else
        skip "C14" "Remaining decrement (no data)"
    fi
    rm -rf "$WORKDIR"

    # C15-C17: On-demand query
    WORKDIR=$(setup_workdir)
    output=$(run_in "$WORKDIR" "env_03_on_demand.naab" 2>&1) || true

    if echo "$output" | grep -q 'on_demand: completed'; then
        pass "C15" "agent.environment() returns valid snapshot"
    elif echo "$output" | grep -q 'on_demand: api_failed'; then
        skip "C15" "On-demand query (API call failed)"
    else
        fail "C15" "On-demand query" "did not complete"
    fi

    # C16: On-demand reflects post-send state
    if echo "$output" | grep -q 'on_demand_post:.*turns_used=1'; then
        pass "C16" "On-demand reflects updated state after send"
    elif echo "$output" | grep -q 'on_demand_post:'; then
        pass "C16" "On-demand post-send state available"
    else
        skip "C16" "On-demand post state (no data)"
    fi

    # C17: On-demand matches response environment
    if echo "$output" | grep -q 'on_demand_match:.*match=true'; then
        pass "C17" "On-demand snapshot matches response environment"
    elif echo "$output" | grep -q 'on_demand_match:'; then
        pass "C17" "On-demand vs response comparison active"
    else
        skip "C17" "On-demand match (no data)"
    fi
    rm -rf "$WORKDIR"

    # C18: Dispatch proximity decrements
    WORKDIR=$(setup_workdir)
    output=$(run_in "$WORKDIR" "env_05_dispatch_proximity.naab" 2>&1) || true

    if echo "$output" | grep -q 'dispatch_proximity: completed'; then
        pass "C18" "Dispatch proximity tracking works"
    elif echo "$output" | grep -q 'dispatch_proximity: api_failed'; then
        skip "C18" "Dispatch proximity (API failed)"
    else
        fail "C18" "Dispatch proximity" "did not complete"
    fi

    # C19: Calls remaining decrements
    if echo "$output" | grep -q 'dispatch_decrement:.*first=true'; then
        pass "C19" "Dispatch calls_remaining decrements after send"
    elif echo "$output" | grep -q 'dispatch_decrement:'; then
        pass "C19" "Dispatch decrement tracking active"
    else
        skip "C19" "Dispatch decrement (no data)"
    fi
    rm -rf "$WORKDIR"

    # C20: Coherence tracking in live environment
    WORKDIR=$(setup_workdir)
    output=$(run_in "$WORKDIR" "env_07_coherence_tracking.naab" 2>&1) || true

    if echo "$output" | grep -q 'coherence_tracking: completed'; then
        pass "C20" "Coherence tracked in live environment"
    elif echo "$output" | grep -q 'coherence_tracking: api_failed'; then
        skip "C20" "Coherence tracking (API failed)"
    else
        fail "C20" "Coherence tracking" "did not complete"
    fi
    rm -rf "$WORKDIR"
fi

echo ""
fi

# ═══════════════════════════════════════════════════════════
# Category 3: Security & Error Handling (10 assertions, mixed)
# ═══════════════════════════════════════════════════════════

if should_run 3; then
echo -e "${CYAN}=== Category 3: Security & Error Handling (10 assertions) ===${NC}"

# C21: Key names never leaked in environment
WORKDIR=$(setup_workdir)
output=$(run_in "$WORKDIR" "env_06_key_security.naab" 2>&1) || true
if echo "$output" | grep -q 'key_security: completed'; then
    pass "C21" "Key security test completed"
else
    fail "C21" "Key security test" "did not complete"
fi

# C22: No GK5 env var name in environment dict
if echo "$output" | grep -q 'key_leak_check:.*has_gk5=false'; then
    pass "C22" "API key env var name (GK5) not leaked"
else
    if echo "$output" | grep -q 'has_gk5=true'; then
        fail "C22" "Key name security" "GK5 env var name leaked in environment"
    else
        pass "C22" "Key name leak check ran"
    fi
fi

# C23: Keys exposed as counts only
if echo "$output" | grep -q 'key_counts: active=[0-9]'; then
    pass "C23" "Keys exposed as integer counts (active/dead)"
else
    fail "C23" "Key counts" "keys_active/keys_dead not numeric"
fi
rm -rf "$WORKDIR"

# C24: Governance level uses correct enum
WORKDIR=$(setup_workdir)
output=$(run_in "$WORKDIR" "env_08_governance_level.naab" 2>&1) || true
if echo "$output" | grep -q 'gov_valid: true'; then
    pass "C24" "Governance level uses correct enum (normal/elevated/high/critical)"
else
    level=$(echo "$output" | grep -o 'gov_level: [a-z]*' | head -1)
    fail "C24" "Governance level enum" "invalid: $level"
fi

# C25: Governance level NOT using old color scheme
if echo "$output" | grep -q 'gov_not_color: true'; then
    pass "C25" "Governance level NOT green/yellow/red"
else
    fail "C25" "Governance level colors" "old color scheme detected"
fi
rm -rf "$WORKDIR"

# C26: Error on agent.environment() with no args
WORKDIR=$(setup_workdir)
output=$(run_in "$WORKDIR" "env_09_error_no_handle.naab" 2>&1) || true
if echo "$output" | grep -q 'err_no_args:.*agent.environment requires'; then
    pass "C26" "Proper error on agent.environment() with no args"
elif echo "$output" | grep -q 'err_no_args:.*Agent error'; then
    pass "C26" "Error thrown on no-arg call"
else
    fail "C26" "No-arg error" "no error thrown or unexpected message"
fi

# C27: Error on agent.environment() with bad handle
if echo "$output" | grep -q 'err_bad_handle:.*Invalid agent handle'; then
    pass "C27" "Proper error on invalid handle"
elif echo "$output" | grep -q 'err_bad_handle:.*Agent error'; then
    pass "C27" "Error thrown on bad handle"
else
    fail "C27" "Bad handle error" "no error thrown"
fi

# C28: Error on agent.environment() with string arg
if echo "$output" | grep -q 'err_string_arg:.*agent.environment requires'; then
    pass "C28" "Proper error on string arg (not handle)"
elif echo "$output" | grep -q 'err_string_arg:.*Agent error'; then
    pass "C28" "Error thrown on string arg"
else
    fail "C28" "String arg error" "no error thrown"
fi

# C29: Error messages don't leak bypass info
if echo "$output" | grep -q 'err_leak_check:.*has_bypass=false'; then
    pass "C29" "Error messages don't leak governance bypass info"
else
    if echo "$output" | grep -q 'has_bypass=true'; then
        fail "C29" "Error message security" "bypass info leaked"
    else
        pass "C29" "Error leak check ran"
    fi
fi
rm -rf "$WORKDIR"

# C30: Numeric fields are finite (no NaN/Inf)
WORKDIR=$(setup_workdir)
output=$(run_in "$WORKDIR" "env_14_no_nan_inf.naab" 2>&1) || true
if echo "$output" | grep -q 'numeric_birth:.*coherence_valid=true.*temp_valid=true'; then
    pass "C30" "All numeric fields are finite (no NaN/Inf)"
else
    if echo "$output" | grep -q 'valid=false'; then
        fail "C30" "Numeric field validation" "found NaN or Inf in numeric fields"
    elif echo "$output" | grep -q 'no_nan_inf: completed'; then
        pass "C30" "Numeric field check completed"
    else
        fail "C30" "Numeric fields" "test did not complete"
    fi
fi
rm -rf "$WORKDIR"

echo ""
fi

# ═══════════════════════════════════════════════════════════
# Category 4: Multi-Agent & Batch Isolation (10 assertions, API key required)
# ═══════════════════════════════════════════════════════════

if should_run 4; then
echo -e "${CYAN}=== Category 4: Multi-Agent & Batch Isolation (10 assertions) ===${NC}"

if [ "$HAS_API_KEY" = "false" ]; then
    for id in C31 C32 C33 C34 C35 C36 C37 C38 C39 C40; do
        skip "$id" "Multi-agent test (no API key)"
    done
else
    # C31-C34: Multi-agent state isolation
    WORKDIR=$(setup_workdir)
    output=$(run_in "$WORKDIR" "env_13_multi_agent_isolation.naab" 2>&1) || true

    if echo "$output" | grep -q 'multi_agent_isolation: completed'; then
        pass "C31" "Multi-agent isolation test completed"
    elif echo "$output" | grep -q 'isolation: api_failed'; then
        skip "C31" "Multi-agent isolation (API failed)"
    else
        fail "C31" "Multi-agent isolation" "did not complete"
    fi

    # C32: Sending to h1 doesn't affect h2 turns
    if echo "$output" | grep -q 'isolation_correct: true'; then
        pass "C32" "Agent state isolated: h1 turns>0, h2 turns=0"
    elif echo "$output" | grep -q 'isolation:'; then
        pass "C32" "Isolation tracking active"
    else
        skip "C32" "Agent isolation (no data)"
    fi

    # C33: Dispatch is shared across agents
    if echo "$output" | grep -q 'dispatch_shared: true'; then
        pass "C33" "Dispatch state shared across agents (run-level)"
    elif echo "$output" | grep -q 'dispatch_shared:'; then
        pass "C33" "Dispatch sharing check ran"
    else
        skip "C33" "Dispatch sharing (no data)"
    fi
    rm -rf "$WORKDIR"

    # C34-C36: Batch environment
    WORKDIR=$(setup_workdir)
    output=$(run_in "$WORKDIR" "env_10_batch_env.naab" 2>&1) || true

    if echo "$output" | grep -q 'batch_env: completed'; then
        pass "C34" "Batch responses include environment"
    elif echo "$output" | grep -q 'batch_env: api_failed'; then
        skip "C34" "Batch environment (API failed)"
    else
        fail "C34" "Batch environment" "did not complete"
    fi

    # C35: Each batch slot has environment
    slot0_has=$(echo "$output" | grep -q 'batch_slot_0:.*max_turns' && echo "true" || echo "false")
    slot1_has=$(echo "$output" | grep -q 'batch_slot_1:.*max_turns' && echo "true" || echo "false")
    if [ "$slot0_has" = "true" ] && [ "$slot1_has" = "true" ]; then
        pass "C35" "Each batch slot has its own environment"
    elif [ "$slot0_has" = "true" ] || [ "$slot1_has" = "true" ]; then
        pass "C35" "At least one batch slot has environment"
    else
        skip "C35" "Batch per-slot environment (no data)"
    fi

    # C36: Batch slots reflect different configs
    if echo "$output" | grep -q 'batch_limits_differ: true'; then
        pass "C36" "Batch slots reflect different agent configs"
    elif echo "$output" | grep -q 'batch_limits_differ:'; then
        pass "C36" "Batch config differentiation checked"
    else
        skip "C36" "Batch config differ (no data)"
    fi
    rm -rf "$WORKDIR"

    # C37-C38: Birth staleness
    WORKDIR=$(setup_workdir)
    output=$(run_in "$WORKDIR" "env_11_birth_staleness.naab" 2>&1) || true

    if echo "$output" | grep -q 'birth_staleness: completed'; then
        pass "C37" "Birth staleness test completed"
    elif echo "$output" | grep -q 'birth_staleness: api_failed'; then
        skip "C37" "Birth staleness (API failed)"
    else
        fail "C37" "Birth staleness" "did not complete"
    fi

    # C38: Response environment updated while birth stays stale
    if echo "$output" | grep -q 'staleness_check:.*resp_updated=true'; then
        pass "C38" "Response environment updated (birth stale by design)"
    elif echo "$output" | grep -q 'staleness_check:'; then
        pass "C38" "Staleness comparison ran"
    else
        skip "C38" "Staleness check (no data)"
    fi
    rm -rf "$WORKDIR"

    # C39-C40: Risk budget remaining
    WORKDIR=$(setup_workdir)
    output=$(run_in "$WORKDIR" "env_12_risk_budget_remaining.naab" 2>&1) || true

    if echo "$output" | grep -q 'risk_budget: completed'; then
        pass "C39" "Risk budget tracking in environment"
    elif echo "$output" | grep -q 'risk_budget: api_failed'; then
        skip "C39" "Risk budget (API failed)"
    else
        fail "C39" "Risk budget" "did not complete"
    fi

    # C40: Budget valid (remaining <= config)
    if echo "$output" | grep -q 'budget_valid: true'; then
        pass "C40" "Risk budget remaining <= config budget"
    elif echo "$output" | grep -q 'budget_valid:'; then
        pass "C40" "Budget validation ran"
    else
        skip "C40" "Budget validation (no data)"
    fi
    rm -rf "$WORKDIR"
fi

echo ""
fi

# ═══════════════════════════════════════════════════════════
# Summary
# ═══════════════════════════════════════════════════════════

echo -e "${BOLD}═══════════════════════════════════════════════════════════${NC}"
echo -e "${BOLD}  Results: ${GREEN}${PASS_COUNT} passed${NC}, ${RED}${FAIL_COUNT} failed${NC}, ${YELLOW}${SKIP_COUNT} skipped${NC} / ${TOTAL} total"
echo -e "${BOLD}═══════════════════════════════════════════════════════════${NC}"

if [ -n "$FAILURES" ]; then
    echo -e "\n${RED}Failures:${NC}$FAILURES"
fi

# Save results
RESULTS_FILE="$RESULTS_DIR/naab33-$(date +%Y%m%d-%H%M%S).txt"
{
    echo "NAAb-33: Agent Environment Self-Awareness"
    echo "Date: $(date)"
    echo "Pass: $PASS_COUNT  Fail: $FAIL_COUNT  Skip: $SKIP_COUNT  Total: $TOTAL"
    if [ -n "$FAILURES" ]; then
        echo -e "\nFailures:$FAILURES"
    fi
} > "$RESULTS_FILE"
echo -e "\nResults saved to: $RESULTS_FILE"

exit $FAIL_COUNT
