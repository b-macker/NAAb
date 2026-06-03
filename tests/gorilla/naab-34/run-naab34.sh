#!/usr/bin/env bash
# NAAb-34: Continuous Governance Gorilla Test
# 20 assertions across 4 categories testing all 6 continuous governance features:
#   1. Handle anti-forge (HMAC nonce bypass vectors)
#   2. Action matrix enforcement (blocked + allowed actions)
#   3. Config acceptance + environment fields (temporal decay, adaptive baseline, step-up)
#   4. Multi-agent isolation (restricted vs unrestricted coexistence)
#
# Usage: bash run-naab34.sh [--cat N]

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../../build/naab-lang"
TEST_TMP="${TMPDIR:-/tmp}/naab34-$$"
RESULTS_DIR="$SCRIPT_DIR/results"
SIGNING_KEY="$HOME/.naab/keys/signing.pem"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
NC='\033[0m'
BOLD='\033[1m'

# Category selection (0 = all)
RUN_CAT=0
if [ "${1:-}" = "--cat" ] && [ -n "${2:-}" ]; then
    RUN_CAT="$2"
fi
should_run() { [ "$RUN_CAT" -eq 0 ] || [ "$RUN_CAT" -eq "$1" ]; }

# Counters
PASS_COUNT=0
FAIL_COUNT=0
SKIP_COUNT=0
TOTAL=0
FAILURES=""

pass() {
    local id="$1" desc="$2"
    PASS_COUNT=$((PASS_COUNT + 1))
    TOTAL=$((TOTAL + 1))
    echo -e "  ${GREEN}PASS${NC} [$id] $desc"
}

fail() {
    local id="$1" desc="$2" detail="${3:-}"
    FAIL_COUNT=$((FAIL_COUNT + 1))
    TOTAL=$((TOTAL + 1))
    echo -e "  ${RED}FAIL${NC} [$id] $desc"
    if [ -n "$detail" ]; then
        echo -e "       ${RED}→ $detail${NC}"
    fi
    FAILURES="${FAILURES}\n  [$id] $desc${detail:+ — $detail}"
}

skip() {
    local id="$1" desc="$2"
    SKIP_COUNT=$((SKIP_COUNT + 1))
    TOTAL=$((TOTAL + 1))
    echo -e "  ${YELLOW}SKIP${NC} [$id] $desc"
}

cleanup() { rm -rf "$TEST_TMP"; }
trap cleanup EXIT

mkdir -p "$TEST_TMP" "$RESULTS_DIR"

# Export API key env vars
export GK5 2>/dev/null || true
export NAAB34_BOGUS="AIzaSy-BOGUS-KEY-INVALID-xxxxxxxxx"

# Check binary exists
if [ ! -x "$NAAB" ]; then
    echo -e "${RED}Error: naab-lang binary not found at $NAAB${NC}"
    echo "Run: cd ~/.naab/language/build && cmake .. && make naab-lang -j4"
    exit 1
fi

# Setup workdir with signed govern.json
setup_workdir() {
    local phase_config="$1"
    local workdir="$TEST_TMP/work-$$-$RANDOM"
    mkdir -p "$workdir"
    cp "$SCRIPT_DIR/phases/$phase_config" "$workdir/govern.json"
    if [ -f "$SIGNING_KEY" ]; then
        (cd "$workdir" && NAAB_SIGNING_KEY="$SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
    fi
    echo "$workdir"
}

run_in() {
    local workdir="$1"
    local naab_file="$2"
    cp "$SCRIPT_DIR/src/$naab_file" "$workdir/"
    GK5="$GK5" NAAB34_BOGUS="$NAAB34_BOGUS" \
        "$NAAB" --governance-dashboard "$workdir/$naab_file" 2>&1
}

echo ""
echo -e "${BOLD}═══════════════════════════════════════════════════════════${NC}"
echo -e "${BOLD}  NAAb-34: Continuous Governance Gorilla Test${NC}"
echo -e "${BOLD}═══════════════════════════════════════════════════════════${NC}"
echo ""

# ─── Category 1: Handle Anti-Forge Advanced (A1-A5) ─────────────
if should_run 1; then
    echo -e "${CYAN}Category 1: Handle Anti-Forge Advanced${NC}"
    WORKDIR=$(setup_workdir "phase1-continuous-governance.json")
    output=$(run_in "$WORKDIR" "cg_01_handle_forge.naab" 2>&1) || true

    # A1: Each handle gets unique nonce
    if echo "$output" | grep -q 'cg01_nonce_unique: true'; then
        pass "A1" "Each handle gets a unique __nonce"
    else
        fail "A1" "Each handle gets a unique __nonce" "nonces were identical"
    fi

    # A2: Swapped nonce (from another handle) rejected
    if echo "$output" | grep -q 'cg01_swap_blocked: true'; then
        pass "A2" "Swapped nonce (cross-handle) rejected"
    else
        fail "A2" "Swapped nonce (cross-handle) rejected"
    fi

    # A3: Mutated nonce (appended char) rejected
    if echo "$output" | grep -q 'cg01_mutate_blocked: true'; then
        pass "A3" "Mutated nonce (appended char) rejected"
    else
        fail "A3" "Mutated nonce (appended char) rejected"
    fi

    # A4: Empty string nonce rejected
    if echo "$output" | grep -q 'cg01_empty_blocked: true'; then
        pass "A4" "Empty string nonce rejected"
    else
        fail "A4" "Empty string nonce rejected"
    fi

    # A5: Forge error doesn't leak HMAC/nonce/secret internals
    if echo "$output" | grep -q 'cg01_swap_leaks_internals: false'; then
        pass "A5" "Forge error doesn't leak HMAC/nonce/secret internals"
    else
        fail "A5" "Forge error doesn't leak HMAC/nonce/secret internals"
    fi
    echo ""
fi

# ─── Category 2: Action Matrix Enforcement (A6-A11) ─────────────
if should_run 2; then
    echo -e "${CYAN}Category 2: Action Matrix Enforcement${NC}"

    # --- Blocked actions ---
    WORKDIR=$(setup_workdir "phase1-continuous-governance.json")
    output=$(run_in "$WORKDIR" "cg_02_action_block.naab" 2>&1) || true

    # A6: Agent without AGENT_SEND created
    if echo "$output" | grep -q 'cg02_created: true'; then
        pass "A6" "Agent without AGENT_SEND created successfully"
    else
        fail "A6" "Agent without AGENT_SEND created successfully"
    fi

    # A7: Send blocked by action matrix
    if echo "$output" | grep -q 'cg02_send_blocked: true'; then
        pass "A7" "Send to agent without AGENT_SEND blocked"
    else
        fail "A7" "Send to agent without AGENT_SEND blocked"
    fi

    # A8: Error mentions action matrix restriction
    if echo "$output" | grep -q 'cg02_error_mentions_restriction: true'; then
        pass "A8" "Block error references action matrix restriction"
    else
        fail "A8" "Block error references action matrix restriction"
    fi

    # A9: Error doesn't leak allowed action list values
    if echo "$output" | grep -q 'cg02_error_leaks_actions: false'; then
        pass "A9" "Block error doesn't leak allowed action list"
    else
        fail "A9" "Block error doesn't leak allowed action list"
    fi

    # --- Allowed actions ---
    WORKDIR2=$(setup_workdir "phase1-continuous-governance.json")
    output2=$(run_in "$WORKDIR2" "cg_03_action_allow.naab" 2>&1) || true

    # A10: AGENT_SEND in matrix = not blocked by matrix
    if echo "$output2" | grep -q 'cg03_send_only_matrix_blocked: false'; then
        pass "A10" "Agent WITH AGENT_SEND not blocked by action matrix"
    else
        fail "A10" "Agent WITH AGENT_SEND not blocked by action matrix"
    fi

    # A11: Empty allowed_actions = not blocked by matrix
    if echo "$output2" | grep -q 'cg03_full_access_matrix_blocked: false'; then
        pass "A11" "Agent with empty allowed_actions not blocked by matrix"
    else
        fail "A11" "Agent with empty allowed_actions not blocked by matrix"
    fi
    echo ""
fi

# ─── Category 3: Config Acceptance & Environment (A12-A16) ──────
if should_run 3; then
    echo -e "${CYAN}Category 3: Config Acceptance & Environment${NC}"
    WORKDIR=$(setup_workdir "phase1-continuous-governance.json")
    output=$(run_in "$WORKDIR" "cg_04_config_env.naab" 2>&1) || true

    # A12: No unknown key warnings
    if echo "$output" | grep -qi 'unknown key\|unknown field\|unrecognized'; then
        fail "A12" "No unknown key warnings for new config fields" "Found unknown key warning"
    else
        pass "A12" "No unknown key warnings for new config fields"
    fi

    # A13: Governance loaded
    if echo "$output" | grep -q 'cg04_config_loaded: true'; then
        pass "A13" "Governance loads with all continuous governance config"
    else
        fail "A13" "Governance loads with all continuous governance config"
    fi

    # A14: Network-restricted agent created
    if echo "$output" | grep -q 'cg04_net_restricted_created: true'; then
        pass "A14" "Agent with network_allowed=false created successfully"
    else
        fail "A14" "Agent with network_allowed=false created successfully"
    fi

    # A15: Environment has challenges_passed
    if echo "$output" | grep -q 'cg04_has_challenges_passed: true'; then
        pass "A15" "Environment includes challenges_passed counter"
    else
        fail "A15" "Environment includes challenges_passed counter"
    fi

    # A16: Environment has challenges_failed
    if echo "$output" | grep -q 'cg04_has_challenges_failed: true'; then
        pass "A16" "Environment includes challenges_failed counter"
    else
        fail "A16" "Environment includes challenges_failed counter"
    fi
    echo ""
fi

# ─── Category 4: Multi-Agent Isolation (A17-A20) ────────────────
if should_run 4; then
    echo -e "${CYAN}Category 4: Multi-Agent Isolation${NC}"
    WORKDIR=$(setup_workdir "phase1-continuous-governance.json")
    output=$(run_in "$WORKDIR" "cg_05_multi_agent.naab" 2>&1) || true

    # A17: Both agents created
    if echo "$output" | grep -q 'cg05_both_created: true'; then
        pass "A17" "Restricted + unrestricted agents coexist"
    else
        fail "A17" "Restricted + unrestricted agents coexist"
    fi

    # A18: Restricted agent blocked
    if echo "$output" | grep -q 'cg05_restricted_blocked: true'; then
        pass "A18" "Restricted agent send blocked (no AGENT_SEND)"
    else
        fail "A18" "Restricted agent send blocked (no AGENT_SEND)"
    fi

    # A19: Unrestricted agent NOT blocked by matrix
    if echo "$output" | grep -q 'cg05_unrestricted_matrix_blocked: false'; then
        pass "A19" "Unrestricted agent send not blocked by action matrix"
    else
        fail "A19" "Unrestricted agent send not blocked by action matrix"
    fi

    # A20: Key health available for multi-key agent
    if echo "$output" | grep -q 'cg05_key_health_available: true'; then
        pass "A20" "Key health available for multi-key agent"
    else
        fail "A20" "Key health available for multi-key agent"
    fi
    echo ""
fi

# ─── Results ────────────────────────────────────────────────────
echo -e "${BOLD}═══════════════════════════════════════════════════════════${NC}"
echo -e "${BOLD}  Results${NC}"
echo -e "${BOLD}═══════════════════════════════════════════════════════════${NC}"
echo ""
echo -e "  Total:   $TOTAL"
echo -e "  Passed:  ${GREEN}$PASS_COUNT${NC}"
echo -e "  Failed:  ${RED}$FAIL_COUNT${NC}"
echo -e "  Skipped: ${YELLOW}$SKIP_COUNT${NC}"

if [ $FAIL_COUNT -gt 0 ]; then
    echo ""
    echo -e "  ${RED}Failures:${NC}"
    echo -e "$FAILURES"
fi
echo ""

# Save results
TIMESTAMP=$(date +%Y%m%d-%H%M%S)
RESULTS_FILE="$RESULTS_DIR/naab34-$TIMESTAMP.txt"
cat > "$RESULTS_FILE" <<EOF
NAAb-34 Gorilla Test Results: Continuous Governance
Date: $(date)
Total: $TOTAL | Pass: $PASS_COUNT | Fail: $FAIL_COUNT | Skip: $SKIP_COUNT
EOF
echo "Results saved to $RESULTS_FILE"

[ $FAIL_COUNT -eq 0 ]
