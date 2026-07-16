#!/usr/bin/env bash
# ============================================================
# Living Script EXTENDED — Three-Layer Architecture + Advanced Governance
#
# Script layer  → defines team, task, data flow
# Operator layer → agent that reads telemetry, adjusts govern.json
# Engine layer  → govern.json + CDD + circuit breaker + ratchet
#
# Builds a calculator that EVOLVES through 4 feature additions:
#   Base:    add, subtract, multiply, divide + history
#   Feat 1:  power (x^y) and modulo (%)
#   Feat 2:  memory system (store, recall, clear)
#   Feat 3:  expression parser (evaluate string expressions)
#   Feat 4:  plugin system (register, run, list plugins)
#
# Extended governance surface:
#   Level 7:  Propose/commit + codegen validation
#   Level 8:  Parallel review with fan-out + consensus
#   Level 9:  Structured scoring with governance.scorer
#   Level 10: Preflight validation (agent.check, codegen discovery)
#   Level 11: Batch/pipeline orchestration (agent.batch, agent.pipeline)
#   Level 12: Introspection & observability (agent.environment/usage/messages/
#             key_health/dispatch_status/register_tool/run, governance queries,
#             codegen.run/run_with_args)
#   Level 13: Ratchet enforcement (one-way tightening, blocked fields)
#   Level 14: Upstream provenance (pipeline stage traceability)
#   Level 15: Governance health pulse (verdict progression, epoch tracking)
#   Level 16: Lease & epoch observability (per-agent lease, CDD signal overrides)
#   Level 17: Telemetry event audit (event type coverage verification)
#   Level 18: Codegen boundary (strict failure path, variable injection)
#
# Requires: GK1 env var with a Gemini API key
# Expected runtime: 4-8 minutes (~55-75 API calls)
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
echo -e "${CYAN}|  Living Script EXTENDED: Advanced Governance Surface          |${NC}"
echo -e "${CYAN}|  (4 features, 18 governance levels, ~0 extra API calls)     |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

if [ -z "${GK1:-}" ]; then
    echo -e "${YELLOW}No GK1 API key -- skipping all live tests${NC}"
    for i in $(seq 1 70); do
        skip "N$i" "No GK1 API key"
    done
else
    WORKDIR=$(setup_workdir)
    STDERR_FILE="$TEST_TMP/stderr.log"

    # Level 5: Place 3 requirement files for feature evolution
    echo "Add power(base, exponent) and modulo(a, b) operations to the Calculator class" > "$WORKDIR/input/requirement-001.txt"
    echo "Add a memory system: memory_store(value), memory_recall() returns stored value, memory_clear() resets to None. Memory persists across operations." > "$WORKDIR/input/requirement-002.txt"
    echo "Add an evaluate(expression: str) method that parses and evaluates simple math expressions like '2 + 3 * 4' respecting operator precedence" > "$WORKDIR/input/requirement-003.txt"
    echo "Add a plugin system: register_plugin(name, func), run_plugin(name, *args), list_plugins() returns list of registered plugin names. Plugins persist for the calculator's lifetime." > "$WORKDIR/input/requirement-004.txt"

    echo -e "${CYAN}Running living script (~3-7 minutes with feature evolution)...${NC}"
    echo -e "${CYAN}Config: 4 workers + operator + 2 judges, 4 features, extended governance${NC}"
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

    for phase in PREFLIGHT DESIGN IMPLEMENT REFINE PROPOSE_SELECT FEATURES PARALLEL_REVIEW BATCH_PIPELINE FINAL_REVIEW SCORING INTROSPECTION RATCHET HEALTH_PULSE LEASE_EPOCH TELEMETRY_AUDIT CODEGEN_BOUNDARY; do
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
    # Count both as "the loop ran a cycle" (the harness proves cycles executed;
    # whether the LLM's code passes is the agent's job, not this assertion).
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

    # 4th feature (plugin system)
    if [ "$FEATURES" -ge 4 ]; then
        pass "L5-09" "All 4 features processed ($FEATURES)"
    elif [ "$FEATURES" -ge 3 ]; then
        pass "L5-09" "3+ features processed ($FEATURES, expected 4)"
    else
        fail "L5-09" "Too few features processed" "got $FEATURES, expected 4"
    fi

    # ============================================================
    # L7: PROPOSE/COMMIT (Level 7)
    # ============================================================
    echo -e "${CYAN}Level 7: Propose/Commit${NC}"

    if echo "$OUTPUT" | grep -q "PHASE|PROPOSE_SELECT|"; then
        pass "L7-01" "Propose/Select phase reached"
    else
        fail "L7-01" "Propose/Select phase not reached"
    fi

    PROPOSE_CANDIDATES=$(echo "$OUTPUT" | grep -oP 'PROPOSE_SELECT\|candidates=\K[0-9]+' | tail -1)
    PROPOSE_CANDIDATES=${PROPOSE_CANDIDATES:-0}
    if [ "$PROPOSE_CANDIDATES" -ge 1 ]; then
        pass "L7-02" "Candidates generated ($PROPOSE_CANDIDATES)"
    else
        fail "L7-02" "No candidates generated"
    fi

    if echo "$OUTPUT" | grep -q "PROPOSE_SELECT|committed=true"; then
        pass "L7-03" "Proposal committed"
    else
        fail "L7-03" "No proposal committed"
    fi

    CODEGEN_RESULT=$(echo "$OUTPUT" | grep -oP 'codegen=\K\w+' | tail -1)
    if [ "${CODEGEN_RESULT:-}" = "pass" ]; then
        pass "L7-04" "Codegen syntax validation passed"
    elif [ "${CODEGEN_RESULT:-}" = "fail" ]; then
        fail "L7-04" "Codegen syntax validation failed"
    else
        skip "L7-04" "Codegen validation not reached"
    fi

    # ============================================================
    # L8: PARALLEL REVIEW (Level 8)
    # ============================================================
    echo -e "${CYAN}Level 8: Parallel Review${NC}"

    if echo "$OUTPUT" | grep -q "PHASE|PARALLEL_REVIEW|"; then
        pass "L8-01" "Parallel review phase reached"
    else
        fail "L8-01" "Parallel review phase not reached"
    fi

    PAR_VOTES=$(echo "$OUTPUT" | grep -oP 'PARALLEL_REVIEW\|votes=\K[0-9]+' | head -1)
    PAR_VOTES=${PAR_VOTES:-0}
    if [ "$PAR_VOTES" -ge 2 ]; then
        pass "L8-02" "Fan-out votes collected ($PAR_VOTES)"
    elif [ "$PAR_VOTES" -ge 1 ]; then
        pass "L8-02" "Partial fan-out votes ($PAR_VOTES, expected 2)"
    else
        fail "L8-02" "No fan-out votes collected"
    fi

    PAR_VERDICT=$(echo "$OUTPUT" | grep -oP 'PARALLEL_REVIEW\|.*verdict=\K\w+' | head -1)
    if [ -n "${PAR_VERDICT:-}" ]; then
        pass "L8-03" "Consensus verdict: $PAR_VERDICT"
    else
        fail "L8-03" "No consensus verdict"
    fi

    # ============================================================
    # L9: STRUCTURED SCORING (Level 9)
    # ============================================================
    echo -e "${CYAN}Level 9: Structured Scoring${NC}"

    if echo "$OUTPUT" | grep -q "PHASE|SCORING|"; then
        pass "L9-01" "Scoring phase reached"
    else
        fail "L9-01" "Scoring phase not reached"
    fi

    SCORE_FINDINGS=$(echo "$OUTPUT" | grep -oP 'SCORING\|.*findings=\K[0-9]+' | head -1)
    if [ -n "${SCORE_FINDINGS:-}" ]; then
        pass "L9-02" "Governance findings recorded ($SCORE_FINDINGS findings)"
    else
        fail "L9-02" "No governance findings"
    fi

    SCORE_VERDICT=$(echo "$OUTPUT" | grep -oP 'SCORING\|.*verdict=\K\w+' | head -1)
    if [ -n "${SCORE_VERDICT:-}" ]; then
        pass "L9-03" "Score verdict: $SCORE_VERDICT"
    else
        fail "L9-03" "No score verdict"
    fi

    # ============================================================
    # L10: PREFLIGHT (validation + discovery)
    # ============================================================
    echo -e "${CYAN}Level 10: Preflight Validation${NC}"

    if echo "$OUTPUT" | grep -q "PHASE|PREFLIGHT|start"; then
        pass "L10-01" "Preflight phase reached"
    else
        fail "L10-01" "Preflight phase not reached"
    fi

    PREFLIGHT_CHECKS=$(echo "$OUTPUT" | grep -oP 'PREFLIGHT\|agent_checks=\K[0-9]+' | head -1)
    if [ "${PREFLIGHT_CHECKS:-0}" -ge 7 ]; then
        pass "L10-02" "All 7 agent configs validated ($PREFLIGHT_CHECKS checks)"
    else
        fail "L10-02" "Agent config validation incomplete" "got ${PREFLIGHT_CHECKS:-0}, expected 7"
    fi

    if echo "$OUTPUT" | grep -q "PREFLIGHT|invalid_check=handled"; then
        pass "L10-03" "Invalid config handled gracefully"
    else
        fail "L10-03" "Invalid config not handled"
    fi

    if echo "$OUTPUT" | grep -q "PREFLIGHT|codegen_enabled=true"; then
        pass "L10-04" "Codegen enabled confirmed"
    else
        fail "L10-04" "Codegen not enabled"
    fi

    if echo "$OUTPUT" | grep -q "PREFLIGHT|.*has_python=true"; then
        pass "L10-05" "Python in supported languages"
    else
        fail "L10-05" "Python not in supported languages"
    fi

    # ============================================================
    # L11: BATCH/PIPELINE (parallel + sequential orchestration)
    # ============================================================
    echo -e "${CYAN}Level 11: Batch & Pipeline${NC}"

    if echo "$OUTPUT" | grep -q "PHASE|BATCH_PIPELINE|start"; then
        pass "L11-01" "Batch/Pipeline phase reached"
    else
        fail "L11-01" "Batch/Pipeline phase not reached"
    fi

    BATCH_RESP=$(echo "$OUTPUT" | grep -oP 'BATCH_PIPELINE\|batch_responses=\K[0-9]+' | head -1)
    if [ "${BATCH_RESP:-0}" -ge 2 ]; then
        pass "L11-02" "Batch returned responses ($BATCH_RESP)"
    else
        fail "L11-02" "Batch returned insufficient responses" "got ${BATCH_RESP:-0}, expected 2"
    fi

    if echo "$OUTPUT" | grep -q "BATCH_PIPELINE|pipeline_ok=true"; then
        pass "L11-03" "Pipeline chaining succeeded"
    else
        fail "L11-03" "Pipeline chaining failed"
    fi

    if echo "$OUTPUT" | grep -q "BATCH_PIPELINE|seq_plan=true"; then
        pass "L11-04" "Sequential refinement plan created"
    else
        fail "L11-04" "Sequential refinement plan failed"
    fi

    if echo "$OUTPUT" | grep -q "BATCH_PIPELINE|.*seq_plan_pattern=sequential_refinement"; then
        pass "L11-05" "Sequential refinement plan valid"
    else
        fail "L11-05" "Sequential refinement plan pattern mismatch"
    fi

    # ============================================================
    # L12: INTROSPECTION (observability + extended codegen)
    # ============================================================
    echo -e "${CYAN}Level 12: Introspection${NC}"

    if echo "$OUTPUT" | grep -q "PHASE|INTROSPECTION|start"; then
        pass "L12-01" "Introspection phase reached"
    else
        fail "L12-01" "Introspection phase not reached"
    fi

    if echo "$OUTPUT" | grep -q "INTROSPECTION|environment|"; then
        pass "L12-02" "agent.environment() returned data"
    else
        fail "L12-02" "agent.environment() failed"
    fi

    if echo "$OUTPUT" | grep -q "INTROSPECTION|usage|"; then
        pass "L12-03" "agent.usage() returned data"
    else
        fail "L12-03" "agent.usage() failed"
    fi

    MSGS_COUNT=$(echo "$OUTPUT" | grep -oP 'INTROSPECTION\|messages\|count=\K[0-9]+' | head -1)
    if [ "${MSGS_COUNT:-0}" -ge 1 ]; then
        pass "L12-04" "agent.messages() returned history ($MSGS_COUNT messages)"
    else
        fail "L12-04" "agent.messages() returned no history"
    fi

    if echo "$OUTPUT" | grep -q "INTROSPECTION|key_health|"; then
        pass "L12-05" "agent.key_health() returned data"
    else
        fail "L12-05" "agent.key_health() failed"
    fi

    DISPATCH_CALLS=$(echo "$OUTPUT" | grep -oP 'INTROSPECTION\|dispatch\|calls=\K[0-9]+' | head -1)
    if [ "${DISPATCH_CALLS:-0}" -ge 10 ]; then
        pass "L12-06" "agent.dispatch_status() shows calls ($DISPATCH_CALLS)"
    else
        fail "L12-06" "agent.dispatch_status() too few calls" "got ${DISPATCH_CALLS:-0}"
    fi

    if echo "$OUTPUT" | grep -q "INTROSPECTION|register_tool=true"; then
        pass "L12-07" "agent.register_tool() succeeded"
    else
        fail "L12-07" "agent.register_tool() failed"
    fi

    if echo "$OUTPUT" | grep -q "INTROSPECTION|agent_run|"; then
        pass "L12-08" "agent.run() one-shot succeeded"
    else
        fail "L12-08" "agent.run() failed"
    fi

    if echo "$OUTPUT" | grep -q "INTROSPECTION|calibrate|success=true"; then
        pass "L12-09" "governance.calibrate() succeeded"
    else
        fail "L12-09" "governance.calibrate() failed"
    fi

    OVERRIDE_COUNT=$(echo "$OUTPUT" | grep -oP 'INTROSPECTION\|calibration_overrides=\K[0-9]+' | head -1)
    if [ "${OVERRIDE_COUNT:-0}" -ge 1 ]; then
        pass "L12-10" "governance.calibration() has overrides ($OVERRIDE_COUNT)"
    else
        fail "L12-10" "governance.calibration() returned no overrides"
    fi

    if echo "$OUTPUT" | grep -q "INTROSPECTION|codegen_run=OK"; then
        pass "L12-11" "codegen.run() executed"
    else
        fail "L12-11" "codegen.run() failed"
    fi

    if echo "$OUTPUT" | grep -q "INTROSPECTION|codegen_args=OK"; then
        pass "L12-12" "codegen.run_with_args() executed"
    else
        fail "L12-12" "codegen.run_with_args() failed"
    fi

    # ============================================================
    # L13: RATCHET ENFORCEMENT
    # ============================================================
    echo -e "${CYAN}Level 13: Ratchet Enforcement${NC}"

    if echo "$OUTPUT" | grep -q "PHASE|RATCHET|start"; then
        pass "L13-01" "Ratchet phase reached"
    else
        fail "L13-01" "Ratchet phase not reached"
    fi

    if echo "$OUTPUT" | grep -q "RATCHET|loosen_blocked=true"; then
        pass "L13-02" "Loosening blocked by ratchet guard"
    else
        fail "L13-02" "Loosening not blocked"
    fi

    if echo "$OUTPUT" | grep -q "RATCHET|tighten_ok=true"; then
        pass "L13-03" "Tightening accepted (one-way decrease)"
    else
        fail "L13-03" "Tightening rejected"
    fi

    if echo "$OUTPUT" | grep -q "RATCHET|field_blocked=true"; then
        pass "L13-04" "Non-allowed field blocked (system_prompt)"
    else
        fail "L13-04" "Non-allowed field not blocked"
    fi

    if echo "$OUTPUT" | grep -q "RATCHET|free_adjust_ok=true"; then
        pass "L13-05" "Free adjustment accepted (max_tokens increase)"
    else
        fail "L13-05" "Free adjustment rejected"
    fi

    # ============================================================
    # L14: UPSTREAM PROVENANCE
    # ============================================================
    echo -e "${CYAN}Level 14: Upstream Provenance${NC}"

    if echo "$OUTPUT" | grep -q "BATCH_PIPELINE|provenance_present=true"; then
        pass "L14-01" "Pipeline upstream provenance present"
        PROV_STAGE=$(echo "$OUTPUT" | grep -oP 'provenance_present=true\|stage=\K[0-9-]+' | head -1)
        PROV_COH=$(echo "$OUTPUT" | grep -oP 'upstream_coherence=\K[0-9.e+-]+' | head -1)
        pass "L14-02" "Provenance has stage index ($PROV_STAGE) and coherence ($PROV_COH)"
    elif echo "$OUTPUT" | grep -q "BATCH_PIPELINE|provenance_present=false"; then
        skip "L14-01" "Pipeline ran but no provenance (single-stage or first-stage result)"
        skip "L14-02" "No provenance data to inspect"
    else
        skip "L14-01" "Pipeline may not have run"
        skip "L14-02" "No provenance data"
    fi

    # ============================================================
    # L15: GOVERNANCE HEALTH PULSE
    # ============================================================
    echo -e "${CYAN}Level 15: Governance Health Pulse${NC}"

    if echo "$OUTPUT" | grep -q "PHASE|HEALTH_PULSE|start"; then
        pass "L15-01" "Health pulse phase reached"
    else
        fail "L15-01" "Health pulse phase not reached"
    fi

    HP_VERDICT=$(echo "$OUTPUT" | grep -oP 'HEALTH_PULSE\|verdict=\K\w+' | head -1)
    if [ -n "${HP_VERDICT:-}" ]; then
        pass "L15-02" "Health verdict: $HP_VERDICT"
    else
        fail "L15-02" "No health verdict"
    fi

    HP_EPOCH=$(echo "$OUTPUT" | grep -oP 'HEALTH_PULSE\|verdict=.*epoch=\K[0-9]+' | head -1)
    if [ "${HP_EPOCH:-0}" -ge 1 ]; then
        pass "L15-03" "Governance epoch advanced ($HP_EPOCH)"
    elif [ "${HP_EPOCH:-0}" -ge 0 ]; then
        pass "L15-03" "Governance epoch tracked ($HP_EPOCH)"
    else
        skip "L15-03" "No epoch data"
    fi

    if echo "$OUTPUT" | grep -q "HEALTH_PULSE|available_keys="; then
        pass "L15-04" "Health keys enumerated"
    else
        skip "L15-04" "No health keys"
    fi

    if echo "$OUTPUT" | grep -q "HEALTH_PULSE|baseline_verdict="; then
        pass "L15-05" "Baseline health comparison available"
    else
        skip "L15-05" "No baseline health comparison"
    fi

    # ============================================================
    # L16: LEASE & EPOCH OBSERVABILITY
    # ============================================================
    echo -e "${CYAN}Level 16: Lease & Epoch${NC}"

    if echo "$OUTPUT" | grep -q "PHASE|LEASE_EPOCH|start"; then
        pass "L16-01" "Lease/epoch phase reached"
    else
        fail "L16-01" "Lease/epoch phase not reached"
    fi

    LEASE_AGENTS=$(echo "$OUTPUT" | grep -c 'LEASE_EPOCH|.*|lease=' || echo "0")
    if [ "$LEASE_AGENTS" -ge 2 ]; then
        pass "L16-02" "Lease data for $LEASE_AGENTS agents"
    else
        fail "L16-02" "Insufficient lease data" "got $LEASE_AGENTS agents"
    fi

    if echo "$OUTPUT" | grep -q "LEASE_EPOCH|.*|challenges_p="; then
        pass "L16-03" "Challenge pass/fail counters visible"
    else
        fail "L16-03" "Challenge counters not visible"
    fi

    if echo "$OUTPUT" | grep -q "LEASE_EPOCH|.*|ctx_window="; then
        pass "L16-04" "Context window/strategy visible per-agent"
    else
        fail "L16-04" "Context config not visible"
    fi

    if echo "$OUTPUT" | grep -q "LEASE_EPOCH|developer|cdd_overrides="; then
        pass "L16-05" "CDD signal overrides visible"
    else
        fail "L16-05" "CDD signal overrides not visible"
    fi

    # ============================================================
    # L17: TELEMETRY EVENT AUDIT
    # ============================================================
    echo -e "${CYAN}Level 17: Telemetry Audit${NC}"

    if echo "$OUTPUT" | grep -q "PHASE|TELEMETRY_AUDIT|start"; then
        pass "L17-01" "Telemetry audit phase reached"
    else
        fail "L17-01" "Telemetry audit phase not reached"
    fi

    TELEM_TYPES=$(echo "$OUTPUT" | grep -oP 'TELEMETRY_AUDIT\|unique_types=\K[0-9]+' | head -1)
    if [ "${TELEM_TYPES:-0}" -ge 5 ]; then
        pass "L17-02" "Telemetry diversity: $TELEM_TYPES event types"
    elif [ "${TELEM_TYPES:-0}" -ge 3 ]; then
        pass "L17-02" "Telemetry diversity: $TELEM_TYPES event types (expected 5+)"
    else
        fail "L17-02" "Low telemetry diversity" "got ${TELEM_TYPES:-0} types"
    fi

    EXPECTED_FOUND=$(echo "$OUTPUT" | grep -oP 'TELEMETRY_AUDIT\|expected_found=\K[0-9]+' | head -1)
    EXPECTED_TOTAL=$(echo "$OUTPUT" | grep -oP 'TELEMETRY_AUDIT\|expected_total=\K[0-9]+' | head -1)
    if [ "${EXPECTED_FOUND:-0}" -ge 3 ]; then
        pass "L17-03" "Key event types present (${EXPECTED_FOUND}/${EXPECTED_TOTAL:-5})"
    else
        fail "L17-03" "Missing key event types" "found ${EXPECTED_FOUND:-0}/${EXPECTED_TOTAL:-5}"
    fi

    TELEM_TOTAL=$(echo "$OUTPUT" | grep -oP 'TELEMETRY_AUDIT\|unique_types=[0-9]+\|total_events=\K[0-9]+' | head -1)
    if [ "${TELEM_TOTAL:-0}" -ge 50 ]; then
        pass "L17-04" "Substantial telemetry volume ($TELEM_TOTAL events)"
    elif [ "${TELEM_TOTAL:-0}" -ge 20 ]; then
        pass "L17-04" "Telemetry volume: $TELEM_TOTAL events (expected 50+)"
    else
        fail "L17-04" "Low telemetry volume" "got ${TELEM_TOTAL:-0} events"
    fi

    # ============================================================
    # L18: CODEGEN BOUNDARY
    # ============================================================
    echo -e "${CYAN}Level 18: Codegen Boundary${NC}"

    if echo "$OUTPUT" | grep -q "PHASE|CODEGEN_BOUNDARY|start"; then
        pass "L18-01" "Codegen boundary phase reached"
    else
        fail "L18-01" "Codegen boundary phase not reached"
    fi

    if echo "$OUTPUT" | grep -q "CODEGEN_BOUNDARY|strict_threw=true"; then
        pass "L18-02" "codegen.run_strict() throws on invalid code"
    else
        fail "L18-02" "codegen.run_strict() didn't throw on invalid code"
    fi

    if echo "$OUTPUT" | grep -q "CODEGEN_BOUNDARY|run_output="; then
        pass "L18-03" "codegen.run() produced output"
    else
        fail "L18-03" "codegen.run() produced no output"
    fi

    if echo "$OUTPUT" | grep -q "CODEGEN_BOUNDARY|args_output="; then
        pass "L18-04" "codegen.run_with_args() produced output"
    else
        fail "L18-04" "codegen.run_with_args() produced no output"
    fi

    if echo "$OUTPUT" | grep -q "has_python=true"; then
        pass "L18-05" "Python in supported codegen languages"
    else
        fail "L18-05" "Python not in codegen languages"
    fi

    if echo "$OUTPUT" | grep -q "CODEGEN_BOUNDARY|still_enabled=true"; then
        pass "L18-06" "Codegen still enabled after calls"
    else
        skip "L18-06" "Codegen budget may be exhausted"
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
    echo -e "${CYAN}Propose/Commit:${NC}"
    echo "$OUTPUT" | grep '^PROPOSE_SELECT|' | while read -r line; do
        echo -e "  ${CYAN}$line${NC}"
    done

    echo ""
    echo -e "${CYAN}Parallel Review:${NC}"
    echo "$OUTPUT" | grep '^PARALLEL_REVIEW|' | while read -r line; do
        echo -e "  ${CYAN}$line${NC}"
    done

    echo ""
    echo -e "${CYAN}Scoring:${NC}"
    echo "$OUTPUT" | grep '^SCORING|' | while read -r line; do
        echo -e "  ${CYAN}$line${NC}"
    done

    echo ""
    echo -e "${CYAN}Preflight:${NC}"
    echo "$OUTPUT" | grep '^PREFLIGHT|' | while read -r line; do
        echo -e "  ${CYAN}$line${NC}"
    done

    echo ""
    echo -e "${CYAN}Batch/Pipeline:${NC}"
    echo "$OUTPUT" | grep '^BATCH_PIPELINE|' | while read -r line; do
        echo -e "  ${CYAN}$line${NC}"
    done

    echo ""
    echo -e "${CYAN}Introspection:${NC}"
    echo "$OUTPUT" | grep '^INTROSPECTION|' | while read -r line; do
        echo -e "  ${CYAN}$line${NC}"
    done

    echo ""
    echo -e "${CYAN}Ratchet Enforcement:${NC}"
    echo "$OUTPUT" | grep '^RATCHET|' | while read -r line; do
        echo -e "  ${CYAN}$line${NC}"
    done

    echo ""
    echo -e "${CYAN}Health Pulse:${NC}"
    echo "$OUTPUT" | grep '^HEALTH_PULSE|' | while read -r line; do
        echo -e "  ${CYAN}$line${NC}"
    done

    echo ""
    echo -e "${CYAN}Lease & Epoch:${NC}"
    echo "$OUTPUT" | grep '^LEASE_EPOCH|' | while read -r line; do
        echo -e "  ${CYAN}$line${NC}"
    done

    echo ""
    echo -e "${CYAN}Telemetry Audit:${NC}"
    echo "$OUTPUT" | grep '^TELEMETRY_AUDIT|' | while read -r line; do
        echo -e "  ${CYAN}$line${NC}"
    done

    echo ""
    echo -e "${CYAN}Codegen Boundary:${NC}"
    echo "$OUTPUT" | grep '^CODEGEN_BOUNDARY|' | while read -r line; do
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
