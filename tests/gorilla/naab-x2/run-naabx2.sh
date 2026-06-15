#!/usr/bin/env bash
# ============================================================================
# NAAb-X2: Multi-Agent App Builder — Gorilla Test
# 50 assertions across 5 categories verifying multi-agent collaboration:
#   Cat 1: Fleet Setup (10, no API)         — create, check, env, key_health, dispatch, action matrix
#   Cat 2: Pipeline Build (10, live API)    — send, pipeline, upstream provenance, extract_code
#   Cat 3: Parallel Review (10, live API)   — fan_out, batch, consensus_vote, enforce_convergence
#   Cat 4: Tool Refinement (10, live API)   — register_tool, integrator+tools, multi-turn, plan
#   Cat 5: Observability (10, live API)     — usage, dispatch_status, env depth, governance, messages
# Usage: bash run-naabx2.sh [--cat N]
# ============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../../build/naab-lang"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
TEST_TMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}/naabx2-$$"
RESULTS_DIR="$SCRIPT_DIR/results"
SIGNING_KEY="$HOME/.naab/keys/signing.pem"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

# Counters
PASS_COUNT=0
FAIL_COUNT=0
SKIP_COUNT=0
TOTAL=0
FAILURES=""

# Category selection
RUN_CAT=0
if [ "${1:-}" = "--cat" ] && [ -n "${2:-}" ]; then
    RUN_CAT="$2"
fi
should_run() { [ "$RUN_CAT" -eq 0 ] || [ "$RUN_CAT" -eq "$1" ]; }

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

cleanup() {
    rm -rf "$TEST_TMP" 2>/dev/null
}
trap cleanup EXIT

# --- Banned patterns (must never leak in output) ---
BANNED_PATTERNS=(
    "--no-governance"
    "--governance-override"
    "--drift-baseline-save"
    "--sign-governance"
    "--sign-baseline"
    "--keygen"
    "NAAB_SIGNING_KEY"
    "NAAB_GOVERN_KEY"
    "soft-mandatory"
    "soft.mandatory"
)

check_banned() {
    local output="$1"
    for pat in "${BANNED_PATTERNS[@]}"; do
        if echo "$output" | grep -qi -- "$pat"; then
            echo "$pat"
            return 0
        fi
    done
    return 1
}

# --- Trust store setup ---
source "$(dirname "$0")/../../helpers/trust_setup.sh"
setup_isolated_trust
trap 'teardown_isolated_trust; rm -rf "$TEST_TMP"' EXIT

"$NAAB" --keygen "$TEST_TMP/test-key.pem" >/dev/null 2>&1 || true
mkdir -p "$TEST_TMP"
"$NAAB" --keygen "$TEST_TMP/test-key.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/test-key.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/test-key.pem"

# Load API keys
source "$HOME/.bashrc" 2>/dev/null || true

setup_workdir() {
    local workdir="$TEST_TMP/work-$$-$RANDOM"
    mkdir -p "$workdir"
    cp "$SCRIPT_DIR/phases/multi-agent.json" "$workdir/govern.json"
    if [ -f "$NAAB_SIGNING_KEY" ]; then
        (cd "$workdir" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
    fi
    echo "$workdir"
}

run_in() {
    local workdir="$1" naab_file="$2"
    cp "$SCRIPT_DIR/src/$naab_file" "$workdir/"
    timeout 300 "$NAAB" --governance-dashboard "$workdir/$naab_file" 2>&1
}

# --- Header ---
echo -e "${BOLD}${CYAN}+----------------------------------------------------------+${NC}"
echo -e "${BOLD}${CYAN}|  NAAb-X2: Multi-Agent App Builder — Gorilla Test          |${NC}"
echo -e "${BOLD}${CYAN}|  50 assertions across 5 categories                        |${NC}"
echo -e "${BOLD}${CYAN}+----------------------------------------------------------+${NC}"
echo ""

# ======================================================================
# CATEGORY 1: Fleet Setup (no API needed)
# ======================================================================
if should_run 1; then
    echo -e "${CYAN}══════════════════════════════════════════════════════${NC}"
    echo -e "${CYAN}  Category 1: Agent Fleet Setup (10 assertions)${NC}"
    echo -e "${CYAN}══════════════════════════════════════════════════════${NC}"
    echo ""

    WORKDIR=$(setup_workdir)
    output=$(run_in "$WORKDIR" "cat1_fleet_setup.naab")
    exit_code=$?

    # Check for banned patterns
    if leaked=$(check_banned "$output"); then
        fail "BAN" "Output leaks bypass hint" "$leaked"
    fi

    echo "$output" | grep -q 'a1_create_architect: true' && \
        pass "A1" "Create architect agent" || \
        fail "A1" "Architect creation failed" "$(echo "$output" | grep 'a1_error' | head -1)"

    echo "$output" | grep -q 'a2_create_coder: true' && \
        pass "A2" "Create coder agent" || \
        fail "A2" "Coder creation failed" "$(echo "$output" | grep 'a2_error' | head -1)"

    echo "$output" | grep -q 'a3_create_reviewer_a: true' && \
        pass "A3" "Create reviewer_a agent" || \
        fail "A3" "Reviewer A creation failed" "$(echo "$output" | grep 'a3_error' | head -1)"

    echo "$output" | grep -q 'a4_create_reviewer_b: true' && \
        pass "A4" "Create reviewer_b agent" || \
        fail "A4" "Reviewer B creation failed" "$(echo "$output" | grep 'a4_error' | head -1)"

    echo "$output" | grep -q 'a5_create_integrator: true' && \
        pass "A5" "Create integrator agent (with tools)" || \
        fail "A5" "Integrator creation failed" "$(echo "$output" | grep 'a5_error' | head -1)"

    echo "$output" | grep -q 'a6_check_valid: true' && \
        pass "A6" "agent.check() pre-flight valid" || \
        fail "A6" "Pre-flight check failed" "$(echo "$output" | grep 'a6_' | head -2)"

    a7_val=$(echo "$output" | grep "a7_key_pool:" | grep -oP 'pool: \K[0-9]+' || echo "0")
    [ "${a7_val:-0}" -ge 1 ] 2>/dev/null && \
        pass "A7" "Key pool: $a7_val configured keys" || \
        fail "A7" "No keys in pool" "got $a7_val"

    echo "$output" | grep -q 'a8_env_config: true' && \
        pass "A8" "Environment has model + provider" || \
        fail "A8" "Environment missing config" "$(echo "$output" | grep 'a8_' | head -2)"

    echo "$output" | grep -q 'a9_dispatch_clean: true' && \
        pass "A9" "Dispatch status starts clean" || \
        fail "A9" "Dispatch not clean" "$(echo "$output" | grep 'a9_' | head -2)"

    echo "$output" | grep -q 'a10_action_matrix: true' && \
        pass "A10" "Action matrix blocks no_send_agent" || \
        fail "A10" "Action matrix not enforced" "$(echo "$output" | grep 'a10_' | head -1)"

    echo ""
fi

# ======================================================================
# CATEGORY 2: Pipeline Build (live API)
# ======================================================================
if should_run 2; then
    echo -e "${CYAN}══════════════════════════════════════════════════════${NC}"
    echo -e "${CYAN}  Category 2: Pipeline Build (10 assertions)${NC}"
    echo -e "${CYAN}══════════════════════════════════════════════════════${NC}"
    echo ""

    # Check for API key
    if [ -z "${GK1:-}" ]; then
        for i in $(seq 1 10); do
            skip "B$i" "No GK1 API key — skipping live tests"
        done
    else
        WORKDIR=$(setup_workdir)
        output=$(run_in "$WORKDIR" "cat2_pipeline_build.naab")
        exit_code=$?

        if leaked=$(check_banned "$output"); then
            fail "BAN" "Output leaks bypass hint" "$leaked"
        fi

        echo "$output" | grep -q 'b1_architect_design: true' && \
            pass "B1" "Architect produces design" || \
            fail "B1" "Architect design failed" "$(echo "$output" | grep 'b1_' | head -2)"

        echo "$output" | grep -q 'b2_trace_model: true' && \
            pass "B2" "Response trace has model info" || \
            fail "B2" "Trace missing model" "$(echo "$output" | grep 'b2_' | head -2)"

        echo "$output" | grep -q 'b3_pipeline_output: true' && \
            pass "B3" "Pipeline architect→coder produces output" || \
            fail "B3" "Pipeline failed" "$(echo "$output" | grep 'b3_' | head -2)"

        echo "$output" | grep -q 'b4_pipeline_success: true' && \
            pass "B4" "Pipeline response success=true" || \
            fail "B4" "Pipeline response not successful" "$(echo "$output" | grep 'b4_' | head -2)"

        echo "$output" | grep -q 'b5_upstream_provenance: true' && \
            pass "B5" "Pipeline has upstream_provenance" || \
            fail "B5" "Upstream provenance missing" "$(echo "$output" | grep 'b5_' | head -2)"

        echo "$output" | grep -q 'b6_provenance_model: true' && \
            pass "B6" "Provenance contains model_used" || \
            fail "B6" "Provenance missing model" "$(echo "$output" | grep 'b6_' | head -2)"

        echo "$output" | grep -q 'b7_extract_code: true' && \
            pass "B7" "extract_code works on coder response" || \
            fail "B7" "Code extraction failed" "$(echo "$output" | grep 'b7_' | head -2)"

        echo "$output" | grep -q 'b8_multi_turn_history: true' && \
            pass "B8" "Multi-turn history grows" || \
            fail "B8" "History not growing" "$(echo "$output" | grep 'b8_' | head -2)"

        echo "$output" | grep -q 'b9_usage_tracked: true' && \
            pass "B9" "Usage tracking (turns + tokens)" || \
            fail "B9" "Usage not tracked" "$(echo "$output" | grep 'b9_' | head -2)"

        echo "$output" | grep -q 'b10_independent_usage: true' && \
            pass "B10" "Independent per-agent usage" || \
            fail "B10" "Usage not independent" "$(echo "$output" | grep 'b10_' | head -2)"
    fi

    echo ""
fi

# ======================================================================
# CATEGORY 3: Parallel Review (live API)
# ======================================================================
if should_run 3; then
    echo -e "${CYAN}══════════════════════════════════════════════════════${NC}"
    echo -e "${CYAN}  Category 3: Parallel Review (10 assertions)${NC}"
    echo -e "${CYAN}══════════════════════════════════════════════════════${NC}"
    echo ""

    if [ -z "${GK1:-}" ]; then
        for i in $(seq 1 10); do
            skip "C$i" "No GK1 API key — skipping live tests"
        done
    else
        WORKDIR=$(setup_workdir)
        output=$(run_in "$WORKDIR" "cat3_parallel_review.naab")
        exit_code=$?

        if leaked=$(check_banned "$output"); then
            fail "BAN" "Output leaks bypass hint" "$leaked"
        fi

        echo "$output" | grep -q 'c1_fan_out_count: true' && \
            pass "C1" "fan_out returns 2 responses" || \
            fail "C1" "fan_out count wrong" "$(echo "$output" | grep 'c1_' | head -2)"

        echo "$output" | grep -q 'c2_both_responded: true' && \
            pass "C2" "Both reviewers have content" || \
            fail "C2" "Missing reviewer content" "$(echo "$output" | grep 'c2_' | head -2)"

        echo "$output" | grep -q 'c3_both_success: true' && \
            pass "C3" "Both responses success=true" || \
            fail "C3" "Reviewer response not successful" "$(echo "$output" | grep 'c3_' | head -2)"

        echo "$output" | grep -q 'c4_consensus_verdict: true' && \
            pass "C4" "Consensus vote has verdict" || \
            fail "C4" "Consensus vote failed" "$(echo "$output" | grep 'c4_' | head -2)"

        echo "$output" | grep -q 'c5_vote_total: true' && \
            pass "C5" "Consensus total = 2" || \
            fail "C5" "Consensus total wrong" "$(echo "$output" | grep 'c5_' | head -2)"

        echo "$output" | grep -q 'c6_has_majority: true' && \
            pass "C6" "Consensus has majority field" || \
            fail "C6" "No majority field" "$(echo "$output" | grep 'c6_' | head -2)"

        echo "$output" | grep -q 'c7_batch_ordered: true' && \
            pass "C7" "batch() returns ordered results" || \
            fail "C7" "Batch results wrong count" "$(echo "$output" | grep 'c7_' | head -2)"

        echo "$output" | grep -q 'c8_batch_independent: true' && \
            pass "C8" "Batch responses are independent" || \
            fail "C8" "Batch responses empty" "$(echo "$output" | grep 'c8_' | head -2)"

        echo "$output" | grep -q 'c9_convergence_pass: true' && \
            pass "C9" "enforce_convergence accepts valid JSON" || \
            fail "C9" "Convergence check failed" "$(echo "$output" | grep 'c9_' | head -2)"

        echo "$output" | grep -q 'c10_convergence_reject: true' && \
            pass "C10" "enforce_convergence rejects missing fields" || \
            fail "C10" "Convergence didn't reject" "$(echo "$output" | grep 'c10_' | head -2)"
    fi

    echo ""
fi

# ======================================================================
# CATEGORY 4: Tool-Assisted Refinement (live API)
# ======================================================================
if should_run 4; then
    echo -e "${CYAN}══════════════════════════════════════════════════════${NC}"
    echo -e "${CYAN}  Category 4: Tool-Assisted Refinement (10 assertions)${NC}"
    echo -e "${CYAN}══════════════════════════════════════════════════════${NC}"
    echo ""

    if [ -z "${GK1:-}" ]; then
        for i in $(seq 1 10); do
            skip "D$i" "No GK1 API key — skipping live tests"
        done
    else
        WORKDIR=$(setup_workdir)
        output=$(run_in "$WORKDIR" "cat4_tool_refinement.naab")
        exit_code=$?

        if leaked=$(check_banned "$output"); then
            fail "BAN" "Output leaks bypass hint" "$leaked"
        fi

        echo "$output" | grep -q 'd1_register_validate: true' && \
            pass "D1" "Register validate_code tool" || \
            fail "D1" "Tool registration failed" "$(echo "$output" | grep 'd1_' | head -2)"

        echo "$output" | grep -q 'd2_register_lint: true' && \
            pass "D2" "Register lint_check tool" || \
            fail "D2" "Lint tool registration failed" "$(echo "$output" | grep 'd2_' | head -2)"

        echo "$output" | grep -q 'd3_integrator_tools: true' && \
            pass "D3" "Integrator env shows tools" || \
            fail "D3" "Integrator tools not visible" "$(echo "$output" | grep 'd3_' | head -2)"

        echo "$output" | grep -q 'd4_integrator_response: true' && \
            pass "D4" "Integrator responds with tool context" || \
            fail "D4" "Integrator response failed" "$(echo "$output" | grep 'd4_' | head -2)"

        echo "$output" | grep -q 'd5_usage_after_tools: true' && \
            pass "D5" "Usage tracked after tool interaction" || \
            fail "D5" "Usage not tracked after tools" "$(echo "$output" | grep 'd5_' | head -2)"

        echo "$output" | grep -q 'd6_multi_turn_refine: true' && \
            pass "D6" "Multi-turn tool refinement" || \
            fail "D6" "Multi-turn refinement failed" "$(echo "$output" | grep 'd6_' | head -2)"

        echo "$output" | grep -q 'd7_turns_growing: true' && \
            pass "D7" "Turn count grows with multi-turn" || \
            fail "D7" "Turn count not growing" "$(echo "$output" | grep 'd7_' | head -2)"

        echo "$output" | grep -q 'd8_refinement_plan: true' && \
            pass "D8" "sequential_refinement creates plan" || \
            fail "D8" "Refinement plan failed" "$(echo "$output" | grep 'd8_' | head -2)"

        echo "$output" | grep -q 'd9_env_state: true' && \
            pass "D9" "Environment shows turns_used" || \
            fail "D9" "Environment missing turns_used" "$(echo "$output" | grep 'd9_' | head -2)"

        echo "$output" | grep -q 'd10_dispatch_tracked: true' && \
            pass "D10" "Dispatch tracks calls + tokens" || \
            fail "D10" "Dispatch not tracked" "$(echo "$output" | grep 'd10_' | head -2)"
    fi

    echo ""
fi

# ======================================================================
# CATEGORY 5: Observability (live API)
# ======================================================================
if should_run 5; then
    echo -e "${CYAN}══════════════════════════════════════════════════════${NC}"
    echo -e "${CYAN}  Category 5: Observability & Governance State (10 assertions)${NC}"
    echo -e "${CYAN}══════════════════════════════════════════════════════${NC}"
    echo ""

    if [ -z "${GK1:-}" ]; then
        for i in $(seq 1 10); do
            skip "E$i" "No GK1 API key — skipping live tests"
        done
    else
        WORKDIR=$(setup_workdir)
        output=$(run_in "$WORKDIR" "cat5_observability.naab")
        exit_code=$?

        if leaked=$(check_banned "$output"); then
            fail "BAN" "Output leaks bypass hint" "$leaked"
        fi

        echo "$output" | grep -q 'e1_usage_complete: true' && \
            pass "E1" "Usage dict has all token fields" || \
            fail "E1" "Usage dict incomplete" "$(echo "$output" | grep 'e1_' | head -2)"

        echo "$output" | grep -q 'e2_dispatch_status: true' && \
            pass "E2" "Dispatch status reflects calls" || \
            fail "E2" "Dispatch status wrong" "$(echo "$output" | grep 'e2_' | head -2)"

        echo "$output" | grep -q 'e3_key_health: true' && \
            pass "E3" "Key health reports active keys" || \
            fail "E3" "Key health failed" "$(echo "$output" | grep 'e3_' | head -2)"

        echo "$output" | grep -q 'e4_governance_level: true' && \
            pass "E4" "Environment has governance_level" || \
            fail "E4" "No governance_level" "$(echo "$output" | grep 'e4_' | head -2)"

        echo "$output" | grep -q 'e5_governance_health: true' && \
            pass "E5" "Environment has governance_health" || \
            fail "E5" "No governance_health" "$(echo "$output" | grep 'e5_' | head -2)"

        echo "$output" | grep -q 'e6_lease_remaining: true' && \
            pass "E6" "Environment has lease_remaining" || \
            fail "E6" "No lease_remaining" "$(echo "$output" | grep 'e6_' | head -2)"

        echo "$output" | grep -q 'e7_dispatch_proximity: true' && \
            pass "E7" "Environment has dispatch proximity" || \
            fail "E7" "No dispatch proximity" "$(echo "$output" | grep 'e7_' | head -2)"

        echo "$output" | grep -q 'e8_independent_tokens: true' && \
            pass "E8" "All 3 agents have independent tokens" || \
            fail "E8" "Tokens not independent" "$(echo "$output" | grep 'e8_' | head -2)"

        echo "$output" | grep -q 'e9_dispatch_tokens: true' && \
            pass "E9" "Dispatch total tokens > 0" || \
            fail "E9" "No dispatch tokens" "$(echo "$output" | grep 'e9_' | head -2)"

        echo "$output" | grep -q 'e10_messages_history: true' && \
            pass "E10" "agent.messages() returns history" || \
            fail "E10" "Messages history failed" "$(echo "$output" | grep 'e10_' | head -2)"
    fi

    echo ""
fi

# ======================================================================
# SUMMARY
# ======================================================================
echo -e "${BOLD}${CYAN}══════════════════════════════════════════════════════${NC}"
echo -e "${BOLD}  NAAb-X2 Results${NC}"
echo -e "${BOLD}${CYAN}══════════════════════════════════════════════════════${NC}"
echo ""
echo -e "  Pass: ${GREEN}$PASS_COUNT${NC}  Fail: ${RED}$FAIL_COUNT${NC}  Skip: ${YELLOW}$SKIP_COUNT${NC}  Total: $TOTAL"

if [ -n "$FAILURES" ]; then
    echo ""
    echo -e "${RED}  Failures:${NC}"
    echo -e "$FAILURES"
fi

# Save results
mkdir -p "$RESULTS_DIR"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
{
    echo "NAAb-X2 Results — $TIMESTAMP"
    echo "Pass: $PASS_COUNT  Fail: $FAIL_COUNT  Skip: $SKIP_COUNT  Total: $TOTAL"
    if [ -n "$FAILURES" ]; then
        echo ""
        echo "Failures:"
        echo -e "$FAILURES"
    fi
} > "$RESULTS_DIR/results_$TIMESTAMP.txt"

echo ""
echo "  Results saved: results/results_$TIMESTAMP.txt"
echo ""

[ "$FAIL_COUNT" -eq 0 ]
