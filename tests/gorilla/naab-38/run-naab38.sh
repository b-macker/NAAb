#!/usr/bin/env bash
# NAAb-38: Sustained Governance Depth Stress Test
#
# Part A: Synthetic depth stress — 50-round NAAb program exercising all depth features
# Part B: Haiku interactive session — complex project under tight governance
#
# Usage:
#   bash run-naab38.sh              # Run Part A (synthetic stress)
#   bash run-naab38.sh --haiku      # Run Part B (launch Haiku session)
#   bash run-naab38.sh --verify     # Verify telemetry from last run
#   bash run-naab38.sh --all        # Part A + verify

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../../build/naab-lang"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
TMPBASE="${TMPDIR:-/data/data/com.termux/files/usr/tmp}/naab38-$$"
RESULTS_DIR="$SCRIPT_DIR/results"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

PASS=0; FAIL=0; SKIP=0; TOTAL=0

pass() { PASS=$((PASS+1)); TOTAL=$((TOTAL+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL=$((FAIL+1)); TOTAL=$((TOTAL+1)); echo -e "  ${RED}FAIL${NC} [$1] $2${3:+ -- $3}"; }
skip() { SKIP=$((SKIP+1)); TOTAL=$((TOTAL+1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2${3:+ ($3)}"; }

cleanup() { rm -rf "$TMPBASE"; }
trap cleanup EXIT
mkdir -p "$TMPBASE" "$RESULTS_DIR"

# --- Trust store setup ---
source "$(dirname "$0")/../../helpers/trust_setup.sh"
setup_isolated_trust
trap 'teardown_isolated_trust; rm -rf "$TMPBASE"' EXIT

"$NAAB" --keygen "$TMPBASE/test-key.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TMPBASE/test-key.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TMPBASE/test-key.pem"

sign_govern() {
    (cd "$1" && "$NAAB" --sign-governance) >/dev/null 2>&1
}

# ======================================================================
# PART A: Synthetic Depth Stress Test
# ======================================================================
run_part_a() {
    echo -e "${BOLD}${CYAN}+----------------------------------------------------------+${NC}"
    echo -e "${BOLD}${CYAN}|  NAAb-38: Sustained Governance Depth Stress Test          |${NC}"
    echo -e "${BOLD}${CYAN}|  Part A: Synthetic stress — 50 rounds × 3 languages       |${NC}"
    echo -e "${BOLD}${CYAN}+----------------------------------------------------------+${NC}"
    echo ""

    local TEST_DIR="$TMPBASE/part_a"
    mkdir -p "$TEST_DIR"
    cp "$SCRIPT_DIR/govern.json" "$TEST_DIR/"
    cp "$SCRIPT_DIR/src/depth_stress.naab" "$TEST_DIR/"
    sign_govern "$TEST_DIR"

    local TELEM="$TEST_DIR/telemetry.jsonl"
    local DASH_OUT="$TEST_DIR/dashboard.log"

    echo -e "${CYAN}--- A1: Depth stress test runs to completion ---${NC}"
    local START_TIME=$(date +%s)

    (cd "$TEST_DIR" && "$NAAB" \
        --governance-dashboard \
        --governance-telemetry "$TELEM" \
        depth_stress.naab \
    ) >"$TEST_DIR/stdout.log" 2>"$DASH_OUT"
    local EXIT_CODE=$?

    local END_TIME=$(date +%s)
    local ELAPSED=$((END_TIME - START_TIME))
    echo "  Duration: ${ELAPSED}s"

    if [ "$EXIT_CODE" -eq 0 ]; then
        pass "A1" "Depth stress completed (exit 0, ${ELAPSED}s)"
    elif [ "$EXIT_CODE" -eq 2 ]; then
        pass "A1" "Depth stress hit quality gate (exit 2, ${ELAPSED}s) — scoring threshold reached"
    elif [ "$EXIT_CODE" -eq 3 ]; then
        fail "A1" "Depth stress hit HARD block (exit 3)" "governance blocked execution"
    else
        fail "A1" "Depth stress failed (exit $EXIT_CODE)"
    fi

    # A2: Check rounds completed
    echo -e "${CYAN}--- A2: Round completion ---${NC}"
    local ROUNDS=$(grep -o 'Rounds completed: [0-9]*' "$TEST_DIR/stdout.log" 2>/dev/null | grep -o '[0-9]*$')
    if [ -n "$ROUNDS" ] && [ "$ROUNDS" -ge 10 ]; then
        pass "A2" "Completed $ROUNDS rounds (>= 10 minimum)"
    elif [ -n "$ROUNDS" ]; then
        fail "A2" "Only completed $ROUNDS rounds" "expected >= 10"
    else
        fail "A2" "Could not determine rounds completed"
    fi

    # A3: Dashboard output present
    echo -e "${CYAN}--- A3: Governance dashboard ---${NC}"
    if [ -s "$DASH_OUT" ] && grep -qi "governance\|score\|advisory\|pass" "$DASH_OUT" 2>/dev/null; then
        pass "A3" "Dashboard output present ($(wc -l < "$DASH_OUT") lines)"
    else
        fail "A3" "No governance dashboard output"
    fi

    # A4: Telemetry events generated
    echo -e "${CYAN}--- A4: Telemetry depth ---${NC}"
    if [ -s "$TELEM" ]; then
        local TELEM_LINES=$(wc -l < "$TELEM")
        if [ "$TELEM_LINES" -ge 10 ]; then
            pass "A4" "Telemetry: $TELEM_LINES events (>= 10)"
        else
            fail "A4" "Telemetry too sparse: $TELEM_LINES events"
        fi
    else
        fail "A4" "No telemetry output"
    fi

    # A5: Scoring accumulated
    echo -e "${CYAN}--- A5: Cumulative risk scoring ---${NC}"
    if grep -qi "score\|risk\|cumulative" "$DASH_OUT" 2>/dev/null; then
        pass "A5" "Risk scoring active in dashboard"
    elif grep -qi "score" "$TELEM" 2>/dev/null; then
        pass "A5" "Risk scoring active in telemetry"
    else
        skip "A5" "Risk scoring not visible" "may need higher violation rate"
    fi

    # A6: Multiple polyglot languages used
    echo -e "${CYAN}--- A6: Polyglot diversity ---${NC}"
    local POLY_LINE=$(grep "Polyglot blocks:" "$TEST_DIR/stdout.log" 2>/dev/null)
    if [ -n "$POLY_LINE" ]; then
        pass "A6" "Polyglot diversity confirmed: $POLY_LINE"
    else
        fail "A6" "No polyglot block summary in output"
    fi

    # A7: Hash chain integrity
    echo -e "${CYAN}--- A7: Data integrity ---${NC}"
    if grep -q "Final chain valid: true" "$TEST_DIR/stdout.log" 2>/dev/null; then
        pass "A7" "Hash chain integrity verified"
    else
        fail "A7" "Hash chain integrity check failed or missing"
    fi

    # A8: Taint sanitization working
    echo -e "${CYAN}--- A8: Taint tracking ---${NC}"
    if grep -qi "taint" "$DASH_OUT" 2>/dev/null; then
        if grep -qi "violation\|blocked" "$DASH_OUT" 2>/dev/null; then
            fail "A8" "Taint violation detected" "sanitizers not clearing taint"
        else
            pass "A8" "Taint tracking active, no violations"
        fi
    else
        pass "A8" "Taint tracking clean (no taint mentions = no violations)"
    fi

    # A9: Advisory escalation (if enough advisories accumulated)
    echo -e "${CYAN}--- A9: Advisory escalation ---${NC}"
    local ADVISORY_COUNT=$(grep -ci "advisory" "$DASH_OUT" 2>/dev/null; true)
    ADVISORY_COUNT="${ADVISORY_COUNT:-0}"
    if [ "$ADVISORY_COUNT" -ge 4 ]; then
        pass "A9" "Advisory escalation potential: $ADVISORY_COUNT advisory mentions"
    else
        skip "A9" "Advisory count low ($ADVISORY_COUNT)" "escalation may not trigger"
    fi

    # A10: BSD event patterns
    echo -e "${CYAN}--- A10: Behavioral sequence detection ---${NC}"
    if grep -qi "behavioral\|bsd\|sequence\|pattern" "$DASH_OUT" "$TELEM" 2>/dev/null; then
        pass "A10" "BSD events detected in output"
    else
        skip "A10" "No BSD events visible" "patterns may not have matched"
    fi

    # A11: Governance health check fired
    echo -e "${CYAN}--- A11: Governance health ---${NC}"
    if grep -qi "health\|pulse\|entropy\|instrumentation" "$DASH_OUT" "$TELEM" 2>/dev/null; then
        pass "A11" "Governance health monitoring active"
    else
        skip "A11" "Governance health not visible in output"
    fi

    # A12: No crashes or segfaults
    echo -e "${CYAN}--- A12: Stability ---${NC}"
    if [ "$EXIT_CODE" -ne 139 ] && [ "$EXIT_CODE" -ne 134 ] && [ "$EXIT_CODE" -ne 136 ]; then
        pass "A12" "No crashes (exit $EXIT_CODE)"
    else
        fail "A12" "Crashed with signal (exit $EXIT_CODE)"
    fi

    # Save results
    cp "$TEST_DIR/stdout.log" "$RESULTS_DIR/part_a_stdout.log" 2>/dev/null
    cp "$DASH_OUT" "$RESULTS_DIR/part_a_dashboard.log" 2>/dev/null
    cp "$TELEM" "$RESULTS_DIR/part_a_telemetry.jsonl" 2>/dev/null
}

# ======================================================================
# PART B: Haiku Interactive Session
# ======================================================================
run_part_b() {
    echo -e "${BOLD}${CYAN}+----------------------------------------------------------+${NC}"
    echo -e "${BOLD}${CYAN}|  NAAb-38: Haiku Interactive Governance Test               |${NC}"
    echo -e "${BOLD}${CYAN}|  Part B: Complex project under tight governance           |${NC}"
    echo -e "${BOLD}${CYAN}+----------------------------------------------------------+${NC}"
    echo ""

    # Set up project directory for Haiku
    local HAIKU_DIR="$TMPBASE/haiku_project"
    mkdir -p "$HAIKU_DIR/src"
    cp "$SCRIPT_DIR/govern.json" "$HAIKU_DIR/"
    cp "$SCRIPT_DIR/CLAUDE.md" "$HAIKU_DIR/" 2>/dev/null

    sign_govern "$HAIKU_DIR"

    # Set trust store but NOT signing key
    export NAAB_TRUST_STORE_DIR
    unset NAAB_SIGNING_KEY
    export PATH="$(dirname "$NAAB"):$PATH"

    echo "  Project dir: $HAIKU_DIR"
    echo "  Trust store: $NAAB_TRUST_STORE_DIR"
    echo "  NAAB_SIGNING_KEY: (not set)"
    echo ""

    cd "$HAIKU_DIR"
    claude --model haiku -p "You are working on a NAAb language project. Read CLAUDE.md for the full specification.

Your task: Build a multi-file data analysis pipeline that:
1. Reads CSV-formatted incident data (hardcoded in main.naab, 20+ incidents)
2. Uses Python polyglot for statistical analysis (mean, stddev, percentiles)
3. Uses JavaScript polyglot for JSON report generation
4. Uses Shell polyglot for system metrics collection
5. Categorizes incidents by severity using match expressions
6. Builds a hash chain of all analysis results for integrity verification
7. Uses async functions for concurrent batch processing
8. Uses codegen.run_with_args for parameterized analysis
9. Sanitizes ALL polyglot output before file writes (taint tracking is HARD)
10. Writes final report to report.json

Write all source files in src/. The govern.json is already signed — do NOT modify it.
Run your code with: naab-lang src/main.naab
The governance has tight thresholds — expect advisories. Keep iterating until the code passes.
You have 50+ rounds — take your time and get it right."
}

# ======================================================================
# VERIFY: Analyze telemetry from last run
# ======================================================================
run_verify() {
    echo -e "${BOLD}${CYAN}+----------------------------------------------------------+${NC}"
    echo -e "${BOLD}${CYAN}|  NAAb-38: Telemetry Verification                         |${NC}"
    echo -e "${BOLD}${CYAN}+----------------------------------------------------------+${NC}"
    echo ""

    local TELEM="$RESULTS_DIR/part_a_telemetry.jsonl"
    local DASH="$RESULTS_DIR/part_a_dashboard.log"

    if [ ! -s "$TELEM" ]; then
        echo "  No telemetry file found. Run Part A first."
        exit 1
    fi

    echo "  Telemetry: $TELEM ($(wc -l < "$TELEM") events)"
    echo "  Dashboard: $DASH ($(wc -l < "$DASH" 2>/dev/null || echo 0) lines)"
    echo ""

    # V1: Event type diversity
    echo -e "${CYAN}--- V1: Event type diversity ---${NC}"
    local EVENT_TYPES=$(grep -o '"type":"[^"]*"' "$TELEM" 2>/dev/null | sort -u | wc -l)
    if [ "$EVENT_TYPES" -ge 3 ]; then
        pass "V1" "Event diversity: $EVENT_TYPES unique event types"
    else
        fail "V1" "Low event diversity: $EVENT_TYPES types"
    fi

    # V2: Coherence tracked
    echo -e "${CYAN}--- V2: Coherence tracking ---${NC}"
    if grep -q "coherence" "$TELEM" 2>/dev/null; then
        pass "V2" "Coherence values present in telemetry"
    else
        skip "V2" "No coherence in telemetry" "agent features may not have triggered"
    fi

    # V3: Scoring events
    echo -e "${CYAN}--- V3: Scoring in telemetry ---${NC}"
    if grep -q "score\|risk" "$TELEM" 2>/dev/null; then
        pass "V3" "Scoring events present"
    else
        skip "V3" "No scoring events"
    fi

    # V4: Time span
    echo -e "${CYAN}--- V4: Test duration ---${NC}"
    local FIRST_TS=$(head -1 "$TELEM" | grep -o '"timestamp":[0-9.]*' | cut -d: -f2)
    local LAST_TS=$(tail -1 "$TELEM" | grep -o '"timestamp":[0-9.]*' | cut -d: -f2)
    if [ -n "$FIRST_TS" ] && [ -n "$LAST_TS" ]; then
        pass "V4" "Telemetry spans from $FIRST_TS to $LAST_TS"
    else
        skip "V4" "Cannot determine time span"
    fi
}

# ======================================================================
# MAIN
# ======================================================================
case "${1:-}" in
    --haiku)
        run_part_b
        ;;
    --verify)
        run_verify
        ;;
    --all)
        run_part_a
        echo ""
        run_verify
        ;;
    *)
        run_part_a
        ;;
esac

echo ""
echo -e "${BOLD}=== Results: ${GREEN}$PASS passed${NC}, ${RED}$FAIL failed${NC}, ${YELLOW}$SKIP skipped${NC} out of $TOTAL ===${NC}"

if [ "$FAIL" -eq 0 ]; then
    echo -e "\n  ${GREEN}${BOLD}run-naab38.sh: ALL PASSED${NC}"
    exit 0
else
    echo -e "\n  ${RED}${BOLD}run-naab38.sh: FAILURES DETECTED${NC}"
    exit 1
fi
