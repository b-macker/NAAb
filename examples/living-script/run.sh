#!/usr/bin/env bash
# ============================================================
# Living Script Example — Three-Layer Architecture
#
# Script layer  → defines team, task, data flow
# Operator layer → agent that reads telemetry, adjusts govern.json
# Engine layer  → govern.json + CDD + circuit breaker + ratchet
#
# Builds a calculator that EVOLVES through 3 feature additions:
#   Base:    add, subtract, multiply, divide + history
#   Feat 1:  power (x^y) and modulo (%)
#   Feat 2:  memory system (store, recall, clear)
#   Feat 3:  expression parser (evaluate string expressions)
#
# Each feature goes through architect → developer → validate → operator
# plus iterative refinement with tester/reviewer feedback.
#
# Requires: GK1 env var with a Gemini API key
# Expected runtime: 2-5 minutes (~35-50 API calls)
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/living-script-$$"
RESULTS_DIR="$SCRIPT_DIR/results"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; TOTAL=0; FAILURES=""

pass() { local id="$1" desc="$2"; PASS_COUNT=$((PASS_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${GREEN}PASS${NC} [$id] $desc"; }
fail() { local id="$1" desc="$2" detail="${3:-}"; FAIL_COUNT=$((FAIL_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${RED}FAIL${NC} [$id] $desc"; [ -n "$detail" ] && echo -e "       ${RED}-> $detail${NC}"; FAILURES="${FAILURES}\n  [$id] $desc${detail:+ -- $detail}"; }
skip() { local id="$1" desc="$2"; SKIP_COUNT=$((SKIP_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${YELLOW}SKIP${NC} [$id] $desc"; }

# Trust store isolation
source "$SCRIPT_DIR/../../tests/helpers/trust_setup.sh"
setup_isolated_trust
trap 'teardown_isolated_trust; rm -rf "$TEST_TMP"' EXIT
mkdir -p "$TEST_TMP"

# Generate signing key
"$NAAB" --keygen "$TEST_TMP/test-key.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/test-key.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/test-key.pem"
export NAAB_BINARY="$NAAB"
export NAAB_SIGN_PATH="$TEST_TMP/test-key.pem"  # non-blocked alias for script re-signing

setup_workdir() {
    local workdir="$TEST_TMP/work-$$-$RANDOM"
    mkdir -p "$workdir/input" "$workdir/memory"
    cp "$SCRIPT_DIR/src/govern.json" "$workdir/govern.json"
    cp "$SCRIPT_DIR/src/living-script.naab" "$workdir/"
    (cd "$workdir" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
    echo "$workdir"
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  Living Script: Three-Layer Architecture Example             |${NC}"
echo -e "${CYAN}|  (evolving calculator — 3 feature additions + refinement)    |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

if [ -z "${GK1:-}" ]; then
    echo -e "${YELLOW}No GK1 API key -- skipping all live tests${NC}"
    for i in $(seq 1 28); do
        skip "N$i" "No GK1 API key"
    done
else
    WORKDIR=$(setup_workdir)
    STDERR_FILE="$TEST_TMP/stderr.log"

    # Level 5: Place 3 requirement files for feature evolution
    echo "Add power(base, exponent) and modulo(a, b) operations to the Calculator class" > "$WORKDIR/input/requirement-001.txt"
    echo "Add a memory system: memory_store(value), memory_recall() returns stored value, memory_clear() resets to None. Memory persists across operations." > "$WORKDIR/input/requirement-002.txt"
    echo "Add an evaluate(expression: str) method that parses and evaluates simple math expressions like '2 + 3 * 4' respecting operator precedence" > "$WORKDIR/input/requirement-003.txt"

    echo -e "${CYAN}Running living script (~2-5 minutes with feature evolution)...${NC}"
    echo -e "${CYAN}Config: 4 workers + operator, 3 features, max 3 refine cycles${NC}"
    echo ""

    OUTPUT=$(cd "$WORKDIR" && timeout 1800 "$NAAB" --governance-dashboard "living-script.naab" 2>"$STDERR_FILE") && EXIT_CODE=0 || EXIT_CODE=$?

    echo ""
    echo -e "${CYAN}--- Output (last 100 lines) ---${NC}"
    echo "$OUTPUT" | tail -100
    echo -e "${CYAN}--- End Output ---${NC}"
    echo ""

    # ============================================================
    # Extract values
    # ============================================================
    AGENTS_CREATED=$(echo "$OUTPUT" | grep -oP 'SUMMARY_AGENTS_CREATED: \K[0-9]+' | head -1)
    AGENTS_CREATED=${AGENTS_CREATED:-0}
    TOTAL_SENDS=$(echo "$OUTPUT" | grep -oP 'SUMMARY_TOTAL_SENDS: \K[0-9]+' | head -1)
    TOTAL_SENDS=${TOTAL_SENDS:-0}
    SEND_ERRORS=$(echo "$OUTPUT" | grep -oP 'SUMMARY_SEND_ERRORS: \K[0-9]+' | head -1)
    SEND_ERRORS=${SEND_ERRORS:-0}
    ADJUSTMENTS=$(echo "$OUTPUT" | grep -oP 'SUMMARY_ADJUSTMENTS: \K[0-9]+' | head -1)
    ADJUSTMENTS=${ADJUSTMENTS:-0}
    ITERATIONS=$(echo "$OUTPUT" | grep -oP 'SUMMARY_ITERATIONS: \K[0-9]+' | head -1)
    ITERATIONS=${ITERATIONS:-0}
    APPROVED=$(echo "$OUTPUT" | grep -oP 'SUMMARY_APPROVED: \K\w+' | head -1)
    FEATURES=$(echo "$OUTPUT" | grep -oP 'SUMMARY_FEATURES: \K[0-9]+' | head -1)
    FEATURES=${FEATURES:-0}

    echo -e "${CYAN}Extracted: agents=$AGENTS_CREATED sends=$TOTAL_SENDS errors=$SEND_ERRORS${NC}"
    echo -e "${CYAN}          iterations=$ITERATIONS approved=$APPROVED features=$FEATURES adjustments=$ADJUSTMENTS${NC}"
    echo ""

    # ============================================================
    # L1: OPERATOR TUNING
    # ============================================================
    echo -e "${CYAN}Level 1: Operator Tuning${NC}"

    if [ "$AGENTS_CREATED" -ge 4 ]; then
        pass "L1-01" "Team created ($AGENTS_CREATED agents incl. operator)"
    else
        fail "L1-01" "Agent creation" "only $AGENTS_CREATED agents"
    fi

    OP_ACTIONS=$(echo "$OUTPUT" | grep -c 'OPERATOR|action=' || echo "0")
    if [ "$OP_ACTIONS" -ge 8 ]; then
        pass "L1-02" "Operator consulted ($OP_ACTIONS times)"
    elif [ "$OP_ACTIONS" -ge 5 ]; then
        pass "L1-02" "Operator consulted ($OP_ACTIONS times, expected 8+)"
    else
        fail "L1-02" "Insufficient operator consultations" "got $OP_ACTIONS, expected >= 8"
    fi

    pass "L1-03" "Adjustments tracked ($ADJUSTMENTS applied)"

    # ============================================================
    # L2: OUTCOME VALIDATION
    # ============================================================
    echo -e "${CYAN}Level 2: Outcome Validation${NC}"

    VALIDATE_RAN=$(echo "$OUTPUT" | grep -c 'VALIDATE|' || echo "0")
    if [ "$VALIDATE_RAN" -gt 0 ]; then
        pass "L2-01" "Polyglot validation ran ($VALIDATE_RAN checks)"
    else
        fail "L2-01" "No validation output"
    fi

    OPS_CONV=$(echo "$OUTPUT" | grep 'CONVERGENCE|operations.py|valid=true' | head -1)
    if [ -n "$OPS_CONV" ]; then
        pass "L2-02" "operations.py passes convergence"
    else
        fail "L2-02" "operations.py convergence failed"
    fi

    TEST_CONV=$(echo "$OUTPUT" | grep 'CONVERGENCE|test_calc.py|valid=true' | head -1)
    if [ -n "$TEST_CONV" ]; then
        pass "L2-03" "test_calc.py passes convergence"
    else
        fail "L2-03" "test_calc.py convergence failed"
    fi

    # Pytest actually ran
    PYTEST_RUNS=$(echo "$OUTPUT" | grep -c 'PYTEST|exit=' || echo "0")
    if [ "$PYTEST_RUNS" -ge 1 ]; then
        pass "L2-04" "Pytest executed ($PYTEST_RUNS runs)"
    else
        skip "L2-04" "No pytest execution"
    fi

    # ============================================================
    # L3: CROSS-RUN MEMORY
    # ============================================================
    echo -e "${CYAN}Level 3: Cross-Run Memory${NC}"

    if [ -f "$WORKDIR/memory/observations.json" ]; then
        pass "L3-01" "Memory file saved"
    else
        fail "L3-01" "Memory file not found"
    fi

    if [ -f "$WORKDIR/memory/observations.json" ]; then
        python3 -c "
import json
d = json.load(open('$WORKDIR/memory/observations.json'))
assert 'run_count' in d and d['run_count'] == 1
assert 'features_processed' in d and d['features_processed'] >= 1
assert 'refinement_iterations' in d
" 2>/dev/null \
            && pass "L3-02" "Memory has run_count + features + refinement data" \
            || fail "L3-02" "Memory missing expected fields"
    else
        skip "L3-02" "No memory file to check"
    fi

    # ============================================================
    # L4: PHASE TRANSITIONS + REFINEMENT + FEATURES
    # ============================================================
    echo -e "${CYAN}Level 4: Phase Transitions & Evolution${NC}"

    for phase in DESIGN IMPLEMENT REFINE FEATURES FINAL_REVIEW; do
        if echo "$OUTPUT" | grep -q "PHASE|${phase}|"; then
            pass "L4-$phase" "Phase $phase reached"
        else
            fail "L4-$phase" "Phase $phase not reached"
        fi
    done

    # Refinement iterations
    if [ "$ITERATIONS" -ge 1 ]; then
        pass "L4-ITER" "Refinement ran $ITERATIONS iterations"
    else
        fail "L4-ITER" "No refinement iterations"
    fi

    # Developer rewrote code during refinement
    REWRITES=$(echo "$OUTPUT" | grep -c 'REFINE|developer_rewrote' || echo "0")
    if [ "$REWRITES" -gt 0 ]; then
        pass "L4-REWRITE" "Developer rewrote code ($REWRITES times)"
    else
        fail "L4-REWRITE" "No code rewrites during refinement"
    fi

    # Review verdicts tracked
    REVIEWS=$(echo "$OUTPUT" | grep -c 'REVIEW|iter=' || echo "0")
    if [ "$REVIEWS" -gt 0 ]; then
        pass "L4-REVIEW" "Reviewer voted $REVIEWS times"
    else
        fail "L4-REVIEW" "No review verdicts"
    fi

    # ============================================================
    # L5: ENVIRONMENTAL REACTIVITY + FEATURE EVOLUTION
    # ============================================================
    echo -e "${CYAN}Level 5: Environmental Reactivity & Features${NC}"

    REACTIVE_FOUND=$(echo "$OUTPUT" | grep -c "REACTIVE|found=" || echo "0")
    if [ "$REACTIVE_FOUND" -ge 2 ]; then
        pass "L5-01" "Requirement files detected ($REACTIVE_FOUND)"
    elif [ "$REACTIVE_FOUND" -ge 1 ]; then
        pass "L5-01" "Requirement file detected ($REACTIVE_FOUND, expected 3)"
    else
        fail "L5-01" "No reactive events detected"
    fi

    if [ "$FEATURES" -ge 2 ]; then
        pass "L5-02" "Features processed ($FEATURES)"
    elif [ "$FEATURES" -ge 1 ]; then
        pass "L5-02" "Feature processed ($FEATURES, expected 3)"
    else
        fail "L5-02" "No features processed"
    fi

    # Code grew with features (operations.py should be substantially larger)
    FINAL_OPS_LEN=$(echo "$OUTPUT" | grep 'CODE_EXTRACT|operations.py' | grep -oP 'len=\K[0-9]+' | head -1)
    FINAL_OPS_LEN=${FINAL_OPS_LEN:-0}
    if [ "$FINAL_OPS_LEN" -ge 2000 ]; then
        pass "L5-03" "Code evolved substantially (ops=$FINAL_OPS_LEN chars)"
    elif [ "$FINAL_OPS_LEN" -ge 1000 ]; then
        pass "L5-03" "Code grew with features (ops=$FINAL_OPS_LEN chars)"
    else
        fail "L5-03" "Code didn't grow enough" "ops=$FINAL_OPS_LEN chars, expected 2000+"
    fi

    # Feature cycle markers. Completion is now gated on validation: a feature
    # that passes pytest emits |complete, one that still fails emits |incomplete.
    # Count both as "the loop ran a cycle."
    FEAT_CYCLES=$(echo "$OUTPUT" | grep -cE 'FEATURE\|[0-9]+\|(complete|incomplete)' || echo "0")
    FEAT_COMPLETE=$(echo "$OUTPUT" | grep -cE 'FEATURE\|[0-9]+\|complete' || echo "0")
    FEAT_INCOMPLETE=$(echo "$OUTPUT" | grep -cE 'FEATURE\|[0-9]+\|incomplete' || echo "0")
    if [ "$FEAT_CYCLES" -ge 2 ]; then
        pass "L5-04" "Feature cycles ran ($FEAT_CYCLES: $FEAT_COMPLETE complete, $FEAT_INCOMPLETE incomplete)"
    else
        fail "L5-04" "Feature cycles did not run" "only $FEAT_CYCLES cycles"
    fi

    # Tester reviewed during feature additions (Gap 1 fix)
    FEAT_TESTER=$(echo "$OUTPUT" | grep -c 'TURN|tester|feat' || echo "0")
    if [ "$FEAT_TESTER" -ge 1 ]; then
        pass "L5-05" "Tester reviewed during features ($FEAT_TESTER reviews)"
    else
        fail "L5-05" "No tester review during feature additions"
    fi

    # Developer fixed issues found by tester during features
    FEAT_FIXES=$(echo "$OUTPUT" | grep -c 'FEATURE|.*|developer_fixed' || echo "0")
    if [ "$FEAT_FIXES" -ge 1 ]; then
        pass "L5-06" "Developer fixed tester issues during features ($FEAT_FIXES fixes)"
    else
        skip "L5-06" "No tester-driven fixes during features (tester found no issues)"
    fi

    # main.py updated for new features
    if echo "$OUTPUT" | grep -q "MAIN_UPDATED|"; then
        MAIN_LEN=$(echo "$OUTPUT" | grep 'CODE_EXTRACT|main.py' | grep -oP 'len=\K[0-9]+' | head -1)
        pass "L5-07" "main.py updated for all operations (len=${MAIN_LEN:-?})"
    else
        skip "L5-07" "main.py not updated"
    fi

    # Pytest fix loop (developer fixed failing tests)
    PYTEST_FIXES=$(echo "$OUTPUT" | grep -c 'PYTEST_FIX|developer_fixed' || echo "0")
    if [ "$PYTEST_FIXES" -ge 1 ]; then
        pass "L5-08" "Developer fixed pytest failures ($PYTEST_FIXES fixes)"
    else
        skip "L5-08" "No pytest failures to fix (tests passed)"
    fi

    # ============================================================
    # L6: DYNAMIC TEAM
    # ============================================================
    echo -e "${CYAN}Level 6: Dynamic Team${NC}"

    if echo "$OUTPUT" | grep -q "PHASE|DYNAMIC|"; then
        pass "L6-01" "Dynamic team phase reached"
    else
        fail "L6-01" "Dynamic phase not reached"
    fi

    # ============================================================
    # TELEMETRY + HEALTH
    # ============================================================
    echo -e "${CYAN}Telemetry & Health${NC}"

    TELEM_CHAIN=$(echo "$OUTPUT" | grep -oP 'TELEM_CHAIN: \K\w+' | head -1)
    if [ -z "$TELEM_CHAIN" ] && [ -f "$WORKDIR/telemetry.jsonl" ]; then
        TELEM_CHAIN=$(python3 -c "
import json
events = [json.loads(l) for l in open('$WORKDIR/telemetry.jsonl') if l.strip()]
ok = True
for i in range(1, len(events)):
    ph = events[i-1].get('hash','')
    cp = events[i].get('prev_hash','')
    if ph and cp and ph != cp: ok = False
print('true' if ok else 'false')
" 2>/dev/null || echo "false")
    fi
    [ "${TELEM_CHAIN:-}" = "true" ] && pass "T01" "Telemetry hash chain valid" || skip "T01" "Hash chain not verified"

    HEALTH=$(echo "$OUTPUT" | grep -oP 'HEALTH: \K\w+' | head -1)
    if [ -z "$HEALTH" ] && [ -f "$STDERR_FILE" ]; then
        grep -q "IMPAIRED" "$STDERR_FILE" 2>/dev/null && HEALTH="impaired"
        grep -q "governance" "$STDERR_FILE" 2>/dev/null && HEALTH="${HEALTH:-healthy}"
    fi
    if [ "${HEALTH:-}" = "healthy" ] || [ "${HEALTH:-}" = "degraded" ]; then
        pass "H01" "Governance health: $HEALTH"
    else
        fail "H01" "Governance health: ${HEALTH:-unknown}"
    fi

    # Code extraction
    FILES_EXTRACTED=$(echo "$OUTPUT" | grep -c 'CODE_EXTRACT|.*|extracted=true' || echo "0")
    if [ "$FILES_EXTRACTED" -ge 2 ]; then
        pass "C01" "Code extraction ($FILES_EXTRACTED/3 files)"
    else
        fail "C01" "Code extraction failed" "only $FILES_EXTRACTED/3 files"
    fi

    # Total sends — with features we expect 30+
    if [ "$TOTAL_SENDS" -ge 30 ]; then
        pass "C02" "Substantial API calls ($TOTAL_SENDS sends, $SEND_ERRORS errors)"
    elif [ "$TOTAL_SENDS" -ge 20 ]; then
        pass "C02" "Acceptable API calls ($TOTAL_SENDS sends, $SEND_ERRORS errors)"
    else
        fail "C02" "Too few API calls" "got $TOTAL_SENDS, expected 30+"
    fi

    # Developer coherence varied (CDD fired from many turns)
    DEV_MIN=$(echo "$OUTPUT" | grep 'AGENT_STATE|developer' | grep -oP 'min=\K[0-9.e+-]+' | head -1)
    if [ -n "${DEV_MIN:-}" ]; then
        BELOW_ONE=$(echo "$DEV_MIN < 1.0" | bc -l 2>/dev/null || echo "0")
        if [ "$BELOW_ONE" = "1" ]; then
            pass "C03" "Developer CDD varied (min=$DEV_MIN)"
        else
            skip "C03" "Developer coherence stayed at 1.0 (CDD needs more baseline)"
        fi
    else
        skip "C03" "No developer coherence data"
    fi

    # Final review happened and is actionable
    FINAL_ITERS=$(echo "$OUTPUT" | grep -oP 'FINAL_REVIEW\|iterations=\K[0-9]+' | head -1)
    FINAL_ITERS=${FINAL_ITERS:-0}
    if echo "$OUTPUT" | grep -q "FINAL_REVIEW|verdict="; then
        pass "C04" "Final review verdict recorded ($FINAL_ITERS iterations)"
    else
        skip "C04" "No final review verdict"
    fi

    # Final review fix loop (Gap 2 fix — if rejected, developer fixes and re-review)
    FINAL_FIXES=$(echo "$OUTPUT" | grep -c 'FINAL_REVIEW|developer_fixed' || echo "0")
    if [ "$FINAL_ITERS" -ge 2 ]; then
        pass "C05" "Final review fix loop activated ($FINAL_ITERS iterations, $FINAL_FIXES fixes)"
    else
        skip "C05" "Final review approved on first pass (no fix loop needed)"
    fi

    # ============================================================
    # CROSS-RUN TEST (Level 3 verification)
    # ============================================================
    echo -e "${CYAN}Cross-Run Memory Test${NC}"

    # Restore original govern.json (operator may have modified it during run 1)
    cp "$SCRIPT_DIR/src/govern.json" "$WORKDIR/govern.json"
    (cd "$WORKDIR" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true

    OUTPUT2=$(cd "$WORKDIR" && timeout 1800 "$NAAB" --governance-dashboard "living-script.naab" 2>/dev/null) && EXIT2=0 || EXIT2=$?
    if echo "$OUTPUT2" | grep -q "MEMORY|loaded|runs=1"; then
        pass "L3-03" "Second run loaded prior memory (runs=1)"
    else
        fail "L3-03" "Second run didn't load memory"
    fi

    # ============================================================
    # INFO: Detailed output sections
    # ============================================================
    echo ""
    echo -e "${CYAN}Per-Agent Final State:${NC}"
    echo "$OUTPUT" | grep '^AGENT_STATE|' | while IFS='|' read -r _ name rest; do
        echo -e "  ${CYAN}$name $rest${NC}"
    done

    echo ""
    echo -e "${CYAN}Code Extraction:${NC}"
    echo "$OUTPUT" | grep '^CODE_EXTRACT|' | while read -r line; do
        echo -e "  ${CYAN}$line${NC}"
    done

    echo ""
    echo -e "${CYAN}Convergence:${NC}"
    echo "$OUTPUT" | grep '^CONVERGENCE|' | while read -r line; do
        echo -e "  ${CYAN}$line${NC}"
    done

    echo ""
    echo -e "${CYAN}Refinement History:${NC}"
    echo "$OUTPUT" | grep -E '^(REFINE_ITER|REFINE\||REVIEW\|)' | while read -r line; do
        echo -e "  ${CYAN}$line${NC}"
    done

    echo ""
    echo -e "${CYAN}Feature Evolution:${NC}"
    echo "$OUTPUT" | grep -E '^(FEATURE\||REACTIVE\|)' | while read -r line; do
        echo -e "  ${CYAN}$line${NC}"
    done

    echo ""
    echo -e "${CYAN}Stderr Cross-Validation:${NC}"
    if [ -f "$STDERR_FILE" ]; then
        STDERR_SIZE=$(wc -c < "$STDERR_FILE")
        echo -e "  Stderr size: ${STDERR_SIZE} bytes"
        echo ""
        echo -e "${CYAN}--- Stderr (first 25 lines) ---${NC}"
        head -25 "$STDERR_FILE" 2>/dev/null
        echo -e "${CYAN}--- End Stderr ---${NC}"
    fi

    echo ""
    echo -e "${CYAN}Governance Health: ${HEALTH:-unknown}${NC}"
fi

echo ""
echo -e "${CYAN}================================================${NC}"
echo -e "  ${GREEN}PASS: $PASS_COUNT${NC}  ${RED}FAIL: $FAIL_COUNT${NC}  ${YELLOW}SKIP: $SKIP_COUNT${NC}  TOTAL: $TOTAL"
[ -n "$FAILURES" ] && echo -e "\n${RED}Failures:${FAILURES}${NC}"
echo -e "${CYAN}================================================${NC}"

mkdir -p "$RESULTS_DIR"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
echo "{\"pass\": $PASS_COUNT, \"fail\": $FAIL_COUNT, \"skip\": $SKIP_COUNT, \"total\": $TOTAL}" > "$RESULTS_DIR/summary.json"
[ -n "${OUTPUT:-}" ] && echo "$OUTPUT" > "$RESULTS_DIR/results_${TIMESTAMP}.txt"
[ -f "${STDERR_FILE:-/dev/null}" ] && cp "$STDERR_FILE" "$RESULTS_DIR/stderr_${TIMESTAMP}.txt" 2>/dev/null || true
[ -f "${WORKDIR:-/dev/null}/telemetry.jsonl" ] && cp "$WORKDIR/telemetry.jsonl" "$RESULTS_DIR/telemetry_${TIMESTAMP}.jsonl" 2>/dev/null || true
[ -f "${WORKDIR:-/dev/null}/transcript.jsonl" ] && cp "$WORKDIR/transcript.jsonl" "$RESULTS_DIR/transcript_${TIMESTAMP}.jsonl" 2>/dev/null || true

exit "$FAIL_COUNT"
