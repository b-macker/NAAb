#!/usr/bin/env bash
# ============================================================
# naab-43: Governance Gauntlet — Live Proof of Reactive Governance
#
# Runs a live agent through escalating governance pressure, then
# reads and verifies its own telemetry. Three evidence channels
# (agent environment dict, telemetry JSONL, stderr dashboard)
# must all agree.
#
# Requires: GK1 env var with a Gemini API key
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/naab43-$$"
RESULTS_DIR="$SCRIPT_DIR/results"

# Colors
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'

# Counters
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; TOTAL=0; FAILURES=""

pass() { local id="$1" desc="$2"; PASS_COUNT=$((PASS_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${GREEN}PASS${NC} [$id] $desc"; }
fail() { local id="$1" desc="$2" detail="${3:-}"; FAIL_COUNT=$((FAIL_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${RED}FAIL${NC} [$id] $desc"; [ -n "$detail" ] && echo -e "       ${RED}→ $detail${NC}"; FAILURES="${FAILURES}\n  [$id] $desc${detail:+ — $detail}"; }
skip() { local id="$1" desc="$2"; SKIP_COUNT=$((SKIP_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${YELLOW}SKIP${NC} [$id] $desc"; }

# Trust store setup
source "$SCRIPT_DIR/../../helpers/trust_setup.sh"
setup_isolated_trust
trap 'teardown_isolated_trust; rm -rf "$TEST_TMP"' EXIT
mkdir -p "$TEST_TMP"

# Generate signing key
"$NAAB" --keygen "$TEST_TMP/test-key.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/test-key.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/test-key.pem"

# Setup workdir with govern.json
setup_workdir() {
    local workdir="$TEST_TMP/work-$$-$RANDOM"
    mkdir -p "$workdir"
    cp "$SCRIPT_DIR/src/govern.json" "$workdir/govern.json"
    (cd "$workdir" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
    echo "$workdir"
}

echo ""
echo -e "${CYAN}╔══════════════════════════════════════════════════════════════╗${NC}"
echo -e "${CYAN}║  naab-43: Governance Gauntlet — Live Reactive Governance    ║${NC}"
echo -e "${CYAN}╚══════════════════════════════════════════════════════════════╝${NC}"
echo ""

# ============================================================
# Check for API key
# ============================================================
if [ -z "${GK1:-}" ]; then
    echo -e "${YELLOW}No GK1 API key — skipping all live gauntlet tests${NC}"
    for i in $(seq 1 22); do
        skip "G$i" "No GK1 API key"
    done
else
    WORKDIR=$(setup_workdir)
    STDERR_FILE="$TEST_TMP/stderr.log"

    # Copy gauntlet.naab and run
    cp "$SCRIPT_DIR/src/gauntlet.naab" "$WORKDIR/"
    echo -e "${CYAN}Running gauntlet (this takes 60-120s with live API calls)...${NC}"

    OUTPUT=$(cd "$WORKDIR" && timeout 300 "$NAAB" --governance-dashboard "gauntlet.naab" 2>"$STDERR_FILE") && EXIT_CODE=0 || EXIT_CODE=$?

    echo ""
    echo -e "${CYAN}--- Gauntlet Output ---${NC}"
    echo "$OUTPUT"
    echo -e "${CYAN}--- End Output ---${NC}"
    echo ""

    # ============================================================
    # PHASE 1 ASSERTIONS: Baseline (3)
    # ============================================================
    echo -e "${CYAN}Phase 1: Baseline${NC}"

    echo "$OUTPUT" | grep -q 'P1_CREATED: true' && \
        pass "G1" "Agent created successfully" || \
        fail "G1" "Agent creation failed" "$(echo "$OUTPUT" | grep 'P1_CREATED\|p1_create_error' | head -1)"

    echo "$OUTPUT" | grep -q 'P1_01_COHERENCE_FRESH: true' && \
        pass "G2" "Fresh coherence >= 0.85" || \
        fail "G2" "Fresh coherence too low" "$(echo "$OUTPUT" | grep 'P1_01' | head -2)"

    echo "$OUTPUT" | grep -q 'P1_02_LEVEL_NORMAL: true' && \
        pass "G3" "Initial governance level is normal" || \
        fail "G3" "Initial level not normal" "$(echo "$OUTPUT" | grep 'P1_02' | head -2)"

    # ============================================================
    # PHASE 2 ASSERTIONS: Coherence Erosion (4)
    # ============================================================
    echo -e "${CYAN}Phase 2: Coherence Erosion${NC}"

    echo "$OUTPUT" | grep -q 'P2_01_COHERENCE_DROPPED: true' && \
        pass "G4" "Coherence dropped after circular prompts" || \
        fail "G4" "Coherence did not drop" "$(echo "$OUTPUT" | grep 'P2_01' | head -2)"

    echo "$OUTPUT" | grep -q 'P2_02_BELOW_BASELINE: true' && \
        pass "G5" "Coherence measurably below 1.0" || \
        fail "G5" "Coherence still near 1.0" "$(echo "$OUTPUT" | grep 'P2_02' | head -1)"

    echo "$OUTPUT" | grep -q 'P2_03_SIGNIFICANT_DROP: true' && \
        pass "G6" "Coherence erosion >= 0.1 (circular weight = 0.15)" || \
        fail "G6" "Erosion too small" "$(echo "$OUTPUT" | grep 'P2_03' | head -2)"

    echo "$OUTPUT" | grep -q 'P2_04_TURNS_COMPLETED: true' && \
        pass "G7" "Multiple circular turns completed" || \
        fail "G7" "Too few turns completed" "$(echo "$OUTPUT" | grep 'P2_04' | head -2)"

    # ============================================================
    # PHASE 3 ASSERTIONS: Pressure Escalation (4)
    # ============================================================
    echo -e "${CYAN}Phase 3: Governance Reacts to Pressure${NC}"

    echo "$OUTPUT" | grep -q 'P3_01_GOVERNANCE_REACTED: true' && \
        pass "G8" "Governance reacted (level change or step-up challenge)" || \
        fail "G8" "No governance reaction" "$(echo "$OUTPUT" | grep 'P3_01' | head -4)"

    echo "$OUTPUT" | grep -q 'P3_02_TURNS_COMPLETED: true' && \
        pass "G9" "Escalation turns completed (>= 2)" || \
        fail "G9" "Too few escalation turns" "$(echo "$OUTPUT" | grep 'P3_02' | head -2)"

    echo "$OUTPUT" | grep -q 'P3_04_ABOVE_BASELINE: true' && \
        pass "G10" "Governance above baseline (level or challenges)" || \
        fail "G10" "Governance stayed at baseline" "$(echo "$OUTPUT" | grep 'P3_04\|P3_01' | head -3)"

    # Check that we can see coherence or challenge data in escalation turns
    ESCALATION_DATA=$(echo "$OUTPUT" | grep -E 'COHERENCE:|CHALLENGES:|p3_send_error' | head -10)
    [ -n "$ESCALATION_DATA" ] && \
        pass "G11" "Governance data visible in escalation turns" || \
        fail "G11" "No governance data in escalation output"

    # ============================================================
    # PHASE 4 ASSERTIONS: Step-Up Challenge (3)
    # ============================================================
    echo -e "${CYAN}Phase 4: Step-Up Challenge${NC}"

    echo "$OUTPUT" | grep -q 'P4_01_CHALLENGE_FIRED: true' && \
        pass "G12" "Step-up challenge was evaluated" || \
        fail "G12" "No challenge fired" "$(echo "$OUTPUT" | grep 'P4_01' | head -4)"

    echo "$OUTPUT" | grep -q 'P4_02_OUTCOME_RECORDED: true' && \
        pass "G13" "Challenge outcome recorded (passed, failed, or threw)" || \
        fail "G13" "No challenge outcome" "$(echo "$OUTPUT" | grep 'P4_02\|P4_01' | head -4)"

    echo "$OUTPUT" | grep -q 'P4_03_OBSERVABLE_EFFECT: true' && \
        pass "G14" "Challenge had observable effect" || \
        fail "G14" "No observable challenge effect" "$(echo "$OUTPUT" | grep 'P4_03' | head -1)"

    # ============================================================
    # PHASE 5 ASSERTIONS: Governance Stops Agent (4)
    # ============================================================
    echo -e "${CYAN}Phase 5: Governance Stops Agent${NC}"

    echo "$OUTPUT" | grep -q 'P5_01_AGENT_STOPPED: true' && \
        pass "G15" "Governance stopped the agent (send threw)" || \
        fail "G15" "Agent not stopped" "$(echo "$OUTPUT" | grep 'P5_01\|P5_TURN' | tail -5)"

    echo "$OUTPUT" | grep -q 'P5_02_GOVERNANCE_ERROR: true' && \
        pass "G16" "Error is governance-caused (budget/challenge/admission)" || \
        fail "G16" "Non-governance error" "$(echo "$OUTPUT" | grep 'P5_02_MSG' | head -1)"

    echo "$OUTPUT" | grep -q 'P5_03_REASONABLE_COUNT: true' && \
        pass "G17" "Total sends in reasonable range (3-35)" || \
        fail "G17" "Send count unreasonable" "$(echo "$OUTPUT" | grep 'P5_03' | head -2)"

    echo "$OUTPUT" | grep -q 'P5_04_AGENT_DEAD: true' && \
        pass "G18" "Agent is effectively dead" || \
        fail "G18" "Agent still alive after governance stop"

    # ============================================================
    # PHASE 6 ASSERTIONS: Telemetry Verification (4)
    # ============================================================
    echo -e "${CYAN}Phase 6: Telemetry Verification${NC}"

    echo "$OUTPUT" | grep -q 'P6_01_HASH_CHAIN: true' && \
        pass "G19" "Telemetry hash chain intact" || \
        fail "G19" "Hash chain broken" "$(echo "$OUTPUT" | grep 'P6_01' | head -2)"

    echo "$OUTPUT" | grep -q 'P6_02_EVENT_DIVERSITY: true' && \
        pass "G20" "4+ distinct telemetry event types" || \
        fail "G20" "Insufficient event diversity" "$(echo "$OUTPUT" | grep 'P6_02' | head -3)"

    echo "$OUTPUT" | grep -q 'P6_03_RUN_ID_CONSISTENT: true' && \
        pass "G21" "All events share same run_id" || \
        fail "G21" "Run ID inconsistency" "$(echo "$OUTPUT" | grep 'P6_03' | head -1)"

    echo "$OUTPUT" | grep -q 'P6_04_TIMESTAMPS_ORDERED: true' && \
        pass "G22" "Timestamps are non-decreasing" || \
        fail "G22" "Timestamp ordering violated" "$(echo "$OUTPUT" | grep 'P6_04' | head -1)"

    # ============================================================
    # STDERR CROSS-VALIDATION
    # ============================================================
    echo ""
    echo -e "${CYAN}Stderr Cross-Validation:${NC}"

    if [ -f "$STDERR_FILE" ]; then
        STDERR_CONTENT=$(cat "$STDERR_FILE")

        # Check for governance dashboard in stderr
        if echo "$STDERR_CONTENT" | grep -qi "Governance\|dashboard\|governance summary\|CDD\|BSD\|coherence"; then
            echo -e "  ${GREEN}✓${NC} Governance activity visible in stderr"
        else
            echo -e "  ${YELLOW}!${NC} No governance activity in stderr (dashboard may not have fired)"
        fi

        # Check for circuit breaker / level change in stderr
        if echo "$STDERR_CONTENT" | grep -qi "level\|elevated\|circuit\|step.up\|challenge"; then
            echo -e "  ${GREEN}✓${NC} Level change / step-up activity in stderr"
        else
            echo -e "  ${YELLOW}!${NC} No level change in stderr"
        fi

        # Show stderr summary (first 20 lines)
        echo ""
        echo -e "${CYAN}--- Stderr (first 30 lines) ---${NC}"
        head -30 "$STDERR_FILE" 2>/dev/null
        echo -e "${CYAN}--- End Stderr ---${NC}"
    else
        echo -e "  ${YELLOW}!${NC} No stderr file captured"
    fi
fi

# ============================================================
# SUMMARY
# ============================================================
echo ""
echo -e "${CYAN}════════════════════════════════════════════════${NC}"
echo -e "  ${GREEN}PASS: $PASS_COUNT${NC}  ${RED}FAIL: $FAIL_COUNT${NC}  ${YELLOW}SKIP: $SKIP_COUNT${NC}  TOTAL: $TOTAL"

if [ -n "$FAILURES" ]; then
    echo -e "\n${RED}Failures:${FAILURES}${NC}"
fi
echo -e "${CYAN}════════════════════════════════════════════════${NC}"

# Save results
mkdir -p "$RESULTS_DIR"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
echo "{\"pass\": $PASS_COUNT, \"fail\": $FAIL_COUNT, \"skip\": $SKIP_COUNT, \"total\": $TOTAL}" > "$RESULTS_DIR/summary.json"

# Save full output for post-mortem
if [ -n "${OUTPUT:-}" ]; then
    echo "$OUTPUT" > "$RESULTS_DIR/results_${TIMESTAMP}.txt"
fi
if [ -f "${STDERR_FILE:-/dev/null}" ]; then
    cp "$STDERR_FILE" "$RESULTS_DIR/stderr_${TIMESTAMP}.txt" 2>/dev/null || true
fi

# Copy telemetry for manual audit
if [ -f "${WORKDIR:-/dev/null}/telemetry.jsonl" ]; then
    cp "$WORKDIR/telemetry.jsonl" "$RESULTS_DIR/telemetry_${TIMESTAMP}.jsonl" 2>/dev/null || true
fi

exit "$FAIL_COUNT"
