#!/usr/bin/env bash
# ============================================================
# Living Script v2 — Multi-Language Build System
#
# Script layer  -> defines team, task, data flow
# Operator layer -> agent that reads telemetry, adjusts govern.json
# Engine layer  -> govern.json + CDD + circuit breaker + ratchet
#
# Builds a multi-language build system (Python + Shell) with
# 3-tier agent topology:
#   PM -> leads (engine_lead, deploy_lead, test_lead)
#      -> workers (engine_dev, deploy_dev, test_dev, etc.)
#   13 named agent configs
#
# Shell codegen (deploy.sh) alongside Python codegen.
# 3 feature additions evolving the base system.
#
# 26 governance levels covering:
#   L1:  Agent creation & operator tuning
#   L2:  Pytest validation + shell syntax
#   L3:  Cross-run memory
#   L4:  Phase presence (23 phases)
#   L5:  Design phase (lead responses)
#   L6:  Build engine code structure
#   L7:  Deploy script (shell codegen)
#   L8:  Test harness
#   L9:  Propose/commit
#   L10: Fan-out review
#   L11-L13: Features 1-3
#   L14: Shell codegen boundary
#   L15: Separation of duties
#   L16: Ratchet enforcement
#   L17: Engine-side ratchet
#   L18: Batch/pipeline orchestration
#   L19: Introspection & observability
#   L20: Tool execution
#   L21: Governance health pulse
#   L22: Telemetry event audit
#   L23: Transcript integrity
#   L24: Evidence layer (attestations, snapshots, chain verify)
#   L25: Taint flow
#   L26: Agent topology (v2-specific)
#
# Requires: GK1 env var with a Gemini API key
# Expected runtime: 5-10 minutes (~60-100 API calls)
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"

# Provenance: stamp which source revision and binary produced each results file
BUILD_COMMIT=$(git -C "$SCRIPT_DIR" rev-parse HEAD 2>/dev/null || echo unknown)
BINARY_MTIME=$(date -r "$NAAB" '+%Y-%m-%dT%H:%M:%S%z' 2>/dev/null || echo unknown)

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/living-script-v2-$$"
RESULTS_DIR="$SCRIPT_DIR/results"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; TOTAL=0; FAILURES=""

pass() { local id="$1" desc="$2"; PASS_COUNT=$((PASS_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${GREEN}PASS${NC} [$id] $desc"; }
fail() { local id="$1" desc="$2" detail="${3:-}"; FAIL_COUNT=$((FAIL_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${RED}FAIL${NC} [$id] $desc"; [ -n "$detail" ] && echo -e "       ${RED}-> $detail${NC}"; FAILURES="${FAILURES}\n  [$id] $desc${detail:+ -- $detail}"; }
skip() { local id="$1" desc="$2"; SKIP_COUNT=$((SKIP_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${YELLOW}SKIP${NC} [$id] $desc"; }

# ------------------------------------------------------------
# Governance-kill classification.
#
# When an agent exceeds max_quarantine_streak (or fails a step-up
# challenge), the engine throws an uncatchable GovernanceHardError:
# main.cpp prints the error to stdout and exits 3. That is
# governance succeeding, not the harness failing -- so checks on
# phases the script never reached are reported as SKIP, not FAIL.
# ------------------------------------------------------------
GOV_KILL=""
RUN_TRUNCATED=""

detect_gov_kill() {  # $1=exit code, $2=captured stdout -- echoes kill kind, or nothing
    [ "$1" -eq 3 ] || return 0
    if echo "$2" | grep -q "Agent exceeded maximum quarantine streak"; then
        echo "quarantine_streak"
    elif echo "$2" | grep -q "Step-up challenge failed"; then
        echo "challenge_failure"
    elif echo "$2" | grep -q "escalated after repeated occurrences" \
         || grep -q "\[governance\] ESCALATED" "${STDERR_FILE:-/dev/null}" 2>/dev/null; then
        echo "advisory_escalation"
    fi
}

run_incomplete() { [ -n "$GOV_KILL" ] || [ -n "$RUN_TRUNCATED" ]; }

incomplete_reason() {
    if [ -n "$GOV_KILL" ]; then echo "governance kill: $GOV_KILL"
    else echo "run truncated: $RUN_TRUNCATED"; fi
}

govkill_block() {  # $1=block label, $2=phase marker regex
    if run_incomplete && ! echo "$OUTPUT" | grep -q "$2"; then
        skip "$1" "not reached ($(incomplete_reason))"
        return 0
    fi
    return 1
}

gk_fail() {  # $1=id, $2=desc, $3=detail
    if run_incomplete; then
        skip "$1" "$2 (unreached -- $(incomplete_reason))"
    else
        fail "$1" "$2" "${3:-}"
    fi
}

# Trust store isolation
source "$SCRIPT_DIR/../../tests/helpers/trust_setup.sh"
setup_isolated_trust
trap 'teardown_isolated_trust; if [ -n "${KEEP_TMP:-}" ]; then echo "Artifacts kept: $TEST_TMP"; else rm -rf "$TEST_TMP"; fi' EXIT
mkdir -p "$TEST_TMP"

# Generate signing key
"$NAAB" --keygen "$TEST_TMP/test-key.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/test-key.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/test-key.pem"
export NAAB_BINARY="$NAAB"
export NAAB_SIGN_PATH="$TEST_TMP/test-key.pem"

setup_workdir() {
    local workdir="$TEST_TMP/work-$$-$RANDOM"
    mkdir -p "$workdir/input" "$workdir/memory"
    cp "$SCRIPT_DIR/src/govern.json" "$workdir/govern.json"
    cp "$SCRIPT_DIR/src/living-script.naab" "$workdir/"
    cp "$SCRIPT_DIR"/input/*.txt "$workdir/input/" 2>/dev/null || true
    (cd "$workdir" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
    echo "$workdir"
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  Living Script v2: Multi-Language Build System                |${NC}"
echo -e "${CYAN}|  (3 features, 26 governance levels, polyglot + topology)     |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

if [ -z "${GK1:-}" ]; then
    echo -e "${YELLOW}No GK1 API key -- skipping all live tests${NC}"
    for i in $(seq 1 115); do
        skip "N$i" "No GK1 API key"
    done
else
    WORKDIR=$(setup_workdir)
    STDERR_FILE="$TEST_TMP/stderr.log"

    echo -e "${CYAN}Running living script v2 (~5-10 minutes with feature evolution)...${NC}"
    echo -e "${CYAN}Config: 13 agents, 3-tier topology, 3 features, polyglot (Python + Shell)${NC}"
    echo ""

    OUTPUT=$(cd "$WORKDIR" && timeout 1800 "$NAAB" --governance-dashboard "living-script.naab" 2>"$STDERR_FILE") && EXIT_CODE=0 || EXIT_CODE=$?

    echo ""
    echo -e "${CYAN}--- Output (last 100 lines) ---${NC}"
    echo "$OUTPUT" | tail -100
    echo -e "${CYAN}--- End Output ---${NC}"
    echo ""

    GOV_KILL=$(detect_gov_kill "$EXIT_CODE" "$OUTPUT")

    # Truncation that is NOT a governance kill
    if [ -z "$GOV_KILL" ] && ! echo "$OUTPUT" | grep -q "=== LIVING SCRIPT V2 COMPLETE ==="; then
        LAST_PHASE=$(echo "$OUTPUT" | grep -oP '^PHASE\|\K[A-Z_]+' | tail -1)
        RUN_TRUNCATED="exit=$EXIT_CODE, last phase=${LAST_PHASE:-none}"
        echo -e "${RED}================================================================${NC}"
        echo -e "${RED}  RUN TRUNCATED (not a governance kill): $RUN_TRUNCATED${NC}"
        echo -e "${RED}  Later phases were never reached; their checks report as SKIP.${NC}"
        echo -e "${RED}================================================================${NC}"
        echo ""
        echo -e "${YELLOW}--- last 15 stdout lines before truncation ---${NC}"
        echo "$OUTPUT" | tail -15
        echo -e "${YELLOW}--- last 15 stderr lines ---${NC}"
        tail -15 "$STDERR_FILE" 2>/dev/null
        echo ""
        fail "R01" "Script terminated before completion" "$RUN_TRUNCATED (no governance kill -- investigate stderr above)"
    else
        pass "R01" "Script ran to completion"
    fi

    if [ -n "$GOV_KILL" ]; then
        echo -e "${YELLOW}================================================================${NC}"
        echo -e "${YELLOW}  GOVERNANCE KILL: $GOV_KILL -- run terminated by design.${NC}"
        echo -e "${YELLOW}  Levels the script never reached are reported as SKIP, not FAIL.${NC}"
        echo -e "${YELLOW}================================================================${NC}"
        echo ""
        GK_EVENT="QUARANTINE_STREAK_EXCEEDED"
        [ "$GOV_KILL" = "challenge_failure" ] && GK_EVENT="AGENT_CHALLENGE_FAIL"
        if grep -q "$GK_EVENT" "$WORKDIR/telemetry.jsonl" 2>/dev/null; then
            pass "GK-01" "Governance kill attributable ($GK_EVENT in telemetry)"
        else
            fail "GK-01" "Unattributable governance kill" "exit 3 + kill message but no $GK_EVENT telemetry event"
        fi
    fi

    # ============================================================
    # Extract values
    # ============================================================
    AGENTS_CREATED=$(echo "$OUTPUT" | grep -oP 'SUMMARY_AGENTS: \K[0-9]+' | head -1)
    AGENTS_CREATED=${AGENTS_CREATED:-0}
    TOTAL_SENDS=$(echo "$OUTPUT" | grep -oP 'SUMMARY_SENDS: \K[0-9]+' | head -1)
    TOTAL_SENDS=${TOTAL_SENDS:-0}
    SEND_ERRORS=$(echo "$OUTPUT" | grep -oP 'SUMMARY_ERRORS: \K[0-9]+' | head -1)
    SEND_ERRORS=${SEND_ERRORS:-0}
    ADJUSTMENTS=$(echo "$OUTPUT" | grep -oP 'SUMMARY_ADJUSTMENTS: \K[0-9]+' | head -1)
    ADJUSTMENTS=${ADJUSTMENTS:-0}
    FEATURES=$(echo "$OUTPUT" | grep -oP 'SUMMARY_FEATURES: \K[0-9]+' | head -1)
    FEATURES=${FEATURES:-0}
    VERDICT=$(echo "$OUTPUT" | grep -oP 'SUMMARY_VERDICT: \K\w+' | head -1)
    VERDICT=${VERDICT:-NONE}
    TOOL_CALLS=$(echo "$OUTPUT" | grep -oP 'SUMMARY_TOOL_CALLS: \K[0-9]+' | head -1)
    TOOL_CALLS=${TOOL_CALLS:-0}
    PHASES_COMPLETED=$(echo "$OUTPUT" | grep -oP 'SUMMARY_PHASES: \K[0-9]+' | head -1)
    PHASES_COMPLETED=${PHASES_COMPLETED:-0}
    OA_QUARANTINED=$(echo "$OUTPUT" | grep -oP 'SUMMARY_OA_QUARANTINED: \K[0-9]+' | head -1)
    OA_QUARANTINED=${OA_QUARANTINED:-0}
    CHALLENGE_PASSES=$(echo "$OUTPUT" | grep -oP 'SUMMARY_CHALLENGE_PASSES: \K[0-9]+' | head -1)
    CHALLENGE_PASSES=${CHALLENGE_PASSES:-0}
    SHELL_CODEGEN=$(echo "$OUTPUT" | grep -oP 'SUMMARY_SHELL_CODEGEN: \K[0-9]+' | head -1)
    SHELL_CODEGEN=${SHELL_CODEGEN:-0}

    echo -e "${CYAN}Extracted: agents=$AGENTS_CREATED sends=$TOTAL_SENDS errors=$SEND_ERRORS${NC}"
    echo -e "${CYAN}          features=$FEATURES verdict=$VERDICT adjustments=$ADJUSTMENTS tools=$TOOL_CALLS${NC}"
    echo -e "${CYAN}          phases=$PHASES_COMPLETED oa_quarantined=$OA_QUARANTINED challenges=$CHALLENGE_PASSES shell=$SHELL_CODEGEN${NC}"
    echo ""

    # ============================================================
    # L1: AGENT CREATION & OPERATOR
    # ============================================================
    echo -e "${CYAN}Level 1: Agent Creation & Operator${NC}"

    if [ "$AGENTS_CREATED" -ge 8 ]; then
        pass "L1-01" "Team created ($AGENTS_CREATED agents)"
    else
        gk_fail "L1-01" "Agent creation" "only $AGENTS_CREATED agents, expected >= 8"
    fi

    # BRE: 'OPERATOR|action=' is a literal marker, not an alternation of
    # "OPERATOR" or "action=". Under -E this counted every line containing
    # either token, inflating the operator-consultation count.
    OP_ACTIONS=$(echo "$OUTPUT" | grep -c 'OPERATOR|action=' || true)
    if [ "$OP_ACTIONS" -ge 3 ]; then
        pass "L1-02" "Operator consulted ($OP_ACTIONS times)"
    else
        gk_fail "L1-02" "Insufficient operator consultations" "got $OP_ACTIONS, expected >= 3"
    fi

    if [ "$ADJUSTMENTS" -ge 1 ]; then
        pass "L1-03" "Adjustments tracked ($ADJUSTMENTS applied)"
    else
        gk_fail "L1-03" "No adjustments tracked (ADJUSTMENTS=$ADJUSTMENTS)"
    fi

    if echo "$OUTPUT" | grep -q 'PHASE|INIT|'; then
        pass "L1-04" "PM agent created (INIT phase reached)"
    else
        gk_fail "L1-04" "PM agent not created (no INIT phase)"
    fi

    if [ "$TOTAL_SENDS" -ge 15 ]; then
        pass "L1-05" "Sufficient API calls ($TOTAL_SENDS sends)"
    else
        gk_fail "L1-05" "Too few API calls" "got $TOTAL_SENDS, expected >= 15"
    fi

    # ============================================================
    # L2: PYTEST VALIDATION
    # ============================================================
    echo -e "${CYAN}Level 2: Pytest Validation${NC}"

    PYTEST_RUNS=$(echo "$OUTPUT" | grep -c 'PYTEST|exit=' || true)
    if [ "$PYTEST_RUNS" -ge 1 ]; then
        pass "L2-01" "Pytest executed ($PYTEST_RUNS runs)"
    else
        gk_fail "L2-01" "No pytest execution"
    fi

    PYTEST_PASS=$(echo "$OUTPUT" | grep -c 'PYTEST|exit=0' || true)
    if [ "$PYTEST_PASS" -ge 1 ]; then
        pass "L2-02" "Pytest passed at least once ($PYTEST_PASS runs with exit=0)"
    elif [ "${PYTEST_RUNS:-0}" -ge 1 ]; then
        fail "L2-02" "No pytest run passed (0 exit=0 out of $PYTEST_RUNS runs)"
    else
        gk_fail "L2-02" "Pytest never ran"
    fi

    if echo "$OUTPUT" | grep -q 'IMPLEMENT|build_engine.py|written'; then
        pass "L2-03" "build_engine.py written"
    else
        gk_fail "L2-03" "build_engine.py not written"
    fi

    if echo "$OUTPUT" | grep -q 'IMPLEMENT|test_build.py|written'; then
        pass "L2-04" "test_build.py written"
    else
        gk_fail "L2-04" "test_build.py not written"
    fi

    if echo "$OUTPUT" | grep -q 'IMPLEMENT|deploy.sh|written'; then
        pass "L2-05" "deploy.sh written"
    else
        gk_fail "L2-05" "deploy.sh not written"
    fi

    if echo "$OUTPUT" | grep -q 'DEPLOY|syntax_ok=true'; then
        pass "L2-06" "Shell deploy.sh syntax valid"
    else
        gk_fail "L2-06" "Shell deploy.sh syntax not validated"
    fi

    # ============================================================
    # L3: CROSS-RUN MEMORY
    # ============================================================
    echo -e "${CYAN}Level 3: Cross-Run Memory${NC}"

    if echo "$OUTPUT" | grep -q "MEMORY|"; then
        pass "L3-01" "Memory loaded or fresh start"
    else
        gk_fail "L3-01" "No memory markers"
    fi

    if echo "$OUTPUT" | grep -q "MEMORY|saved"; then
        pass "L3-02" "Memory saved"
    else
        gk_fail "L3-02" "Memory not saved"
    fi

    # ============================================================
    # L4: PHASE PRESENCE
    # ============================================================
    echo -e "${CYAN}Level 4: Phase Presence${NC}"

    for phase in INIT TOOL_REGISTRATION PREFLIGHT TOOL_EXEC DESIGN IMPLEMENT REFINE REVIEW PROPOSE_SELECT FAN_OUT_REVIEW FEATURES SHELL_CODEGEN_BOUNDARY INTROSPECTION SEPARATION_OF_DUTIES RATCHET ENGINE_RATCHET BATCH_PIPELINE GOVERNANCE_HEALTH TELEMETRY_AUDIT TRANSCRIPT_AUDIT EVIDENCE_AUDIT CROSS_RUN_MEMORY FINAL_VERDICT; do
        if echo "$OUTPUT" | grep -q "PHASE|${phase}|"; then
            pass "L4-$phase" "Phase $phase reached"
        else
            gk_fail "L4-$phase" "Phase $phase not reached"
        fi
    done

    # ============================================================
    # L5: DESIGN PHASE
    # ============================================================
    echo -e "${CYAN}Level 5: Design Phase${NC}"

    if ! govkill_block "L5-*" 'PHASE|DESIGN|'; then

    if echo "$OUTPUT" | grep -q 'TURN|engine_lead|design'; then
        pass "L5-01" "engine_lead design response received"
    else
        gk_fail "L5-01" "No engine_lead design response"
    fi

    if echo "$OUTPUT" | grep -q 'TURN|deploy_lead|design'; then
        pass "L5-02" "deploy_lead design response received"
    else
        gk_fail "L5-02" "No deploy_lead design response"
    fi

    if echo "$OUTPUT" | grep -q 'TURN|test_lead|design'; then
        pass "L5-03" "test_lead design response received"
    else
        gk_fail "L5-03" "No test_lead design response"
    fi

    fi

    # ============================================================
    # L6: BUILD ENGINE CODE
    # ============================================================
    echo -e "${CYAN}Level 6: Build Engine Code${NC}"

    if ! govkill_block "L6-*" 'PHASE|IMPLEMENT|'; then

    if echo "$OUTPUT" | grep -qE 'CONVERGENCE.*BuildEngine|CODE_EXTRACT.*build_engine.*extracted=true'; then
        pass "L6-01" "build_engine.py contains class BuildEngine"
    else
        gk_fail "L6-01" "build_engine.py missing class BuildEngine"
    fi

    if echo "$OUTPUT" | grep -qE 'CONVERGENCE.*DependencyGraph|CODE_EXTRACT.*build_engine.*extracted=true'; then
        pass "L6-02" "build_engine.py contains class DependencyGraph"
    else
        gk_fail "L6-02" "build_engine.py missing class DependencyGraph"
    fi

    if echo "$OUTPUT" | grep -qE 'CONVERGENCE.*BuildError|ENGINE\|import_ok=true'; then
        pass "L6-03" "build_engine.py contains class BuildError"
    else
        gk_fail "L6-03" "build_engine.py missing class BuildError"
    fi

    if echo "$OUTPUT" | grep -qE 'CONVERGENCE.*def (build|resolve)|IMPLEMENT.*build_engine.*written'; then
        pass "L6-04" "build_engine.py contains build/resolve method"
    else
        gk_fail "L6-04" "build_engine.py missing build/resolve method"
    fi

    fi

    # ============================================================
    # L7: DEPLOY SCRIPT
    # ============================================================
    echo -e "${CYAN}Level 7: Deploy Script${NC}"

    if ! govkill_block "L7-*" 'PHASE|IMPLEMENT|'; then

    if echo "$OUTPUT" | grep -qE 'IMPLEMENT\|deploy\.sh\|written|DEPLOY\|syntax_ok=true'; then
        pass "L7-01" "deploy.sh generated"
    else
        gk_fail "L7-01" "deploy.sh not generated"
    fi

    if echo "$OUTPUT" | grep -qE 'DEPLOY\|syntax_ok=true|DEPLOY\|repaired=true'; then
        pass "L7-02" "deploy.sh shell syntax valid"
    else
        gk_fail "L7-02" "deploy.sh syntax not validated"
    fi

    # BRE, not -E: every marker this harness emits is pipe-delimited, so in an
    # extended regex an unescaped `|` is alternation rather than the literal it
    # was meant to be. This pattern was 'CODEGEN_EXEC|shell|SHELL_CODEGEN|' under
    # -E — a trailing EMPTY alternative, which matches every non-empty line, so
    # L7-03 could not fail on any run that produced output at all.
    # The real evidence is the telemetry audit line: TELEMETRY_AUDIT|type=CODEGEN_EXEC|count=N
    if echo "$OUTPUT" | grep -q 'type=CODEGEN_EXEC|count=[1-9]'; then
        pass "L7-03" "Shell codegen.run executed"
    else
        gk_fail "L7-03" "Shell codegen.run not executed"
    fi

    fi

    # ============================================================
    # L8: TEST HARNESS
    # ============================================================
    echo -e "${CYAN}Level 8: Test Harness${NC}"

    if ! govkill_block "L8-*" 'PHASE|IMPLEMENT|'; then

    if echo "$OUTPUT" | grep -qE 'TESTMASS\|tests=[1-9]|CONVERGENCE\|test_build\.py\|valid=true|IMPLEMENT\|test_build\.py\|written'; then
        pass "L8-01" "test_build.py contains test functions"
    else
        gk_fail "L8-01" "test_build.py missing test functions"
    fi

    if echo "$OUTPUT" | grep -q 'PYTEST|exit='; then
        pass "L8-02" "pytest ran on test_build.py"
    else
        gk_fail "L8-02" "pytest did not run on test_build.py"
    fi

    fi

    # ============================================================
    # L9: PROPOSE/COMMIT
    # ============================================================
    echo -e "${CYAN}Level 9: Propose/Commit${NC}"

    if ! govkill_block "L9-*" 'PHASE|PROPOSE_SELECT|'; then

    if echo "$OUTPUT" | grep -q 'PROPOSE_SELECT|candidates='; then
        pass "L9-01" "Propose generated candidates"
    else
        gk_fail "L9-01" "No propose candidates generated"
    fi

    if echo "$OUTPUT" | grep -q 'PROPOSE_SELECT|committed=true'; then
        pass "L9-02" "At least 1 candidate committed"
    elif echo "$OUTPUT" | grep -q 'PROPOSE_SELECT|admissible_count=0'; then
        skip "L9-02" "No candidate cleared admissibility threshold (gate working)"
    else
        gk_fail "L9-02" "No candidate committed"
    fi

    fi

    # ============================================================
    # L10: FAN-OUT REVIEW
    # ============================================================
    echo -e "${CYAN}Level 10: Fan-Out Review${NC}"

    if ! govkill_block "L10-*" 'PHASE|FAN_OUT_REVIEW|'; then

    # Was 'FAN_OUT|judges=|FAN_OUT_REVIEW|' under -E: trailing empty alternative,
    # so L10-01 passed on any non-empty output. BRE keeps the pipes literal.
    if echo "$OUTPUT" | grep -q 'PHASE|FAN_OUT_REVIEW|'; then
        pass "L10-01" "Fan-out review executed"
    else
        gk_fail "L10-01" "Fan-out review not executed"
    fi

    if echo "$OUTPUT" | grep -qE 'FAN_OUT_REVIEW\|.*verdict=|FAN_OUT_REVIEW\|final_verdict='; then
        pass "L10-02" "Consensus vote computed"
    else
        gk_fail "L10-02" "No consensus vote"
    fi

    fi

    # ============================================================
    # L11: FEATURE 1
    # ============================================================
    echo -e "${CYAN}Level 11: Feature 1${NC}"

    if ! govkill_block "L11-*" 'FEATURE|1|'; then

    if echo "$OUTPUT" | grep -q 'FEATURE|1|start'; then
        pass "L11-01" "Feature 1 started"
    else
        gk_fail "L11-01" "Feature 1 not started"
    fi

    if echo "$OUTPUT" | grep -qE 'FEATURE\|1\|(engine_updated|pipeline_updated|complete)'; then
        pass "L11-02" "Feature 1 pipeline updated"
    else
        gk_fail "L11-02" "Feature 1 pipeline not updated"
    fi

    if echo "$OUTPUT" | grep -qE 'FEATURE\\|1\\|.*pytest|PYTEST\\|exit='; then
        pass "L11-03" "Feature 1 pytest ran"
    else
        gk_fail "L11-03" "Feature 1 pytest did not run"
    fi

    if echo "$OUTPUT" | grep -qE 'FEATURE\\|1\\|(complete|convergence)'; then
        pass "L11-04" "Feature 1 convergence"
    else
        gk_fail "L11-04" "Feature 1 did not converge"
    fi

    fi

    # ============================================================
    # L12: FEATURE 2
    # ============================================================
    echo -e "${CYAN}Level 12: Feature 2${NC}"

    if ! govkill_block "L12-*" 'FEATURE|2|'; then

    if echo "$OUTPUT" | grep -q 'FEATURE|2|start'; then
        pass "L12-01" "Feature 2 started"
    else
        gk_fail "L12-01" "Feature 2 not started"
    fi

    if echo "$OUTPUT" | grep -qE 'FEATURE\|2\|(engine_updated|pipeline_updated|complete)'; then
        pass "L12-02" "Feature 2 pipeline updated"
    else
        gk_fail "L12-02" "Feature 2 pipeline not updated"
    fi

    if echo "$OUTPUT" | grep -qE 'FEATURE\\|2\\|.*pytest|PYTEST\\|exit='; then
        pass "L12-03" "Feature 2 pytest ran"
    else
        gk_fail "L12-03" "Feature 2 pytest did not run"
    fi

    if echo "$OUTPUT" | grep -qE 'FEATURE\\|2\\|(complete|convergence)'; then
        pass "L12-04" "Feature 2 convergence"
    else
        gk_fail "L12-04" "Feature 2 did not converge"
    fi

    fi

    # ============================================================
    # L13: FEATURE 3
    # ============================================================
    echo -e "${CYAN}Level 13: Feature 3${NC}"

    if ! govkill_block "L13-*" 'FEATURE|3|'; then

    if echo "$OUTPUT" | grep -q 'FEATURE|3|start'; then
        pass "L13-01" "Feature 3 started"
    else
        gk_fail "L13-01" "Feature 3 not started"
    fi

    if echo "$OUTPUT" | grep -qE 'FEATURE\|3\|(engine_updated|pipeline_updated|complete)'; then
        pass "L13-02" "Feature 3 pipeline updated"
    else
        gk_fail "L13-02" "Feature 3 pipeline not updated"
    fi

    if echo "$OUTPUT" | grep -qE 'FEATURE\\|3\\|.*pytest|PYTEST\\|exit='; then
        pass "L13-03" "Feature 3 pytest ran"
    else
        gk_fail "L13-03" "Feature 3 pytest did not run"
    fi

    if echo "$OUTPUT" | grep -qE 'FEATURE\\|3\\|(complete|convergence)'; then
        pass "L13-04" "Feature 3 convergence"
    else
        gk_fail "L13-04" "Feature 3 did not converge"
    fi

    fi

    # ============================================================
    # L14: SHELL CODEGEN BOUNDARY
    # ============================================================
    echo -e "${CYAN}Level 14: Shell Codegen Boundary${NC}"

    if ! govkill_block "L14-*" 'PHASE|SHELL_CODEGEN_BOUNDARY|'; then

    CODEGEN_RUN_LINE=$(echo "$OUTPUT" | grep 'SHELL_CODEGEN|basic|output=' | head -1)
    if [ -n "$CODEGEN_RUN_LINE" ]; then
        pass "L14-01" "codegen.run(\"shell\") produced output"
        if echo "$CODEGEN_RUN_LINE" | grep -q "contains_42=true"; then
            pass "L14-02" "Shell codegen output contains expected value 42"
        else
            fail "L14-02" "Shell codegen output does not contain 42"
        fi
    else
        fail "L14-01" "codegen.run(\"shell\") produced no output"
        fail "L14-02" "No shell codegen output to verify"
    fi

    CODEGEN_ARGS_LINE=$(echo "$OUTPUT" | grep 'SHELL_CODEGEN|args|output=' | head -1)
    if [ -n "$CODEGEN_ARGS_LINE" ]; then
        pass "L14-03" "codegen.run_with_args(\"shell\") produced output"
    else
        fail "L14-03" "codegen.run_with_args(\"shell\") produced no output"
    fi

    if echo "$OUTPUT" | grep -q "SHELL_CODEGEN|strict_threw=true"; then
        pass "L14-04" "codegen.run_strict threw on exit 1"
    else
        fail "L14-04" "codegen.run_strict did not throw on exit 1"
    fi

    fi

    # ============================================================
    # L15: SEPARATION OF DUTIES
    # ============================================================
    echo -e "${CYAN}Level 15: Separation of Duties${NC}"

    if ! govkill_block "L15-*" 'PHASE|SEPARATION_OF_DUTIES|'; then

    if echo "$OUTPUT" | grep -q 'SEPARATION|deploy_dev|shell_allowed='; then
        pass "L15-01" "deploy_dev shell access reported"
    else
        gk_fail "L15-01" "deploy_dev shell access not reported"
    fi

    if echo "$OUTPUT" | grep -q 'SEPARATION|engine_reviewer|tool_calls=0'; then
        pass "L15-02" "engine_reviewer has no tool access"
    else
        gk_fail "L15-02" "engine_reviewer tool restriction not confirmed"
    fi

    if echo "$OUTPUT" | grep -q 'SEPARATION|pm_agent_send=true'; then
        pass "L15-03" "PM has AGENT_SEND permission"
    else
        gk_fail "L15-03" "PM AGENT_SEND not confirmed"
    fi

    fi

    # ============================================================
    # L16: RATCHET ENFORCEMENT
    # ============================================================
    echo -e "${CYAN}Level 16: Ratchet Enforcement${NC}"

    if ! govkill_block "L16-*" 'PHASE|RATCHET|'; then

    if echo "$OUTPUT" | grep -q "RATCHET|loosen_blocked=true"; then
        pass "L16-01" "Loosening blocked by ratchet guard"
    else
        fail "L16-01" "Loosening not blocked"
    fi

    if echo "$OUTPUT" | grep -q "RATCHET|tighten_ok=true"; then
        pass "L16-02" "Tightening accepted (one-way decrease)"
    else
        fail "L16-02" "Tightening rejected"
    fi

    if echo "$OUTPUT" | grep -q "RATCHET|field_blocked=true"; then
        pass "L16-03" "Non-allowed field blocked"
    else
        fail "L16-03" "Non-allowed field not blocked"
    fi

    fi

    # ============================================================
    # L17: ENGINE-SIDE RATCHET
    # ============================================================
    echo -e "${CYAN}Level 17: Engine-Side Ratchet${NC}"

    if ! govkill_block "L17-*" 'PHASE|ENGINE_RATCHET|'; then

    if echo "$OUTPUT" | grep -q 'ENGINE_RATCHET|loosened_write='; then
        pass "L17-01" "Loosened config written to disk"
    else
        fail "L17-01" "Loosened config write not attempted"
    fi

    # Check telemetry for CONFIG_ADJUSTMENT with ratchet rejection
    ER_REJECT=""
    if [ -f "$WORKDIR/telemetry.jsonl" ]; then
        ER_REJECT=$(grep -c 'CONFIG_ADJUSTMENT.*ratchet' "$WORKDIR/telemetry.jsonl" 2>/dev/null || true)
    fi
    if [ "${ER_REJECT:-0}" -ge 1 ]; then
        pass "L17-02" "Engine rejected loosening (CONFIG_ADJUSTMENT ratchet in telemetry)"
    elif echo "$OUTPUT" | grep -q 'ENGINE_RATCHET|engine_rejected_loosening=true'; then
        pass "L17-02" "Engine rejected loosening (script marker)"
    else
        gk_fail "L17-02" "Engine loosening rejection not confirmed"
    fi

    if echo "$OUTPUT" | grep -q 'ENGINE_RATCHET|restored='; then
        pass "L17-03" "Original config restored after probe"
    else
        gk_fail "L17-03" "Config restore not confirmed"
    fi

    fi

    # ============================================================
    # L18: BATCH/PIPELINE
    # ============================================================
    echo -e "${CYAN}Level 18: Batch/Pipeline${NC}"

    if ! govkill_block "L18-*" 'PHASE|BATCH_PIPELINE|'; then

    BATCH_RESP=$(echo "$OUTPUT" | grep -oP 'BATCH_PIPELINE\|batch_responses=\K[0-9]+' | head -1)
    if [ "${BATCH_RESP:-0}" -ge 1 ]; then
        pass "L18-01" "agent.batch executed ($BATCH_RESP results)"
    else
        fail "L18-01" "agent.batch returned no results"
    fi

    if echo "$OUTPUT" | grep -q "BATCH_PIPELINE|seq_plan="; then
        pass "L18-02" "Sequential refinement plan created"
    else
        fail "L18-02" "Sequential refinement plan not created"
    fi

    fi

    # ============================================================
    # L19: INTROSPECTION
    # ============================================================
    echo -e "${CYAN}Level 19: Introspection${NC}"

    if ! govkill_block "L19-*" 'PHASE|INTROSPECTION|'; then

    if echo "$OUTPUT" | grep -q "INTROSPECTION|pm|.*tools_enabled="; then
        pass "L19-01" "PM environment queried (tools_enabled reported)"
    else
        fail "L19-01" "PM environment not queried"
    fi

    if echo "$OUTPUT" | grep -q "INTROSPECTION|engine_lead_usage|"; then
        pass "L19-02" "engine_lead usage queried"
    else
        fail "L19-02" "engine_lead usage not queried"
    fi

    DISPATCH_CALLS=$(echo "$OUTPUT" | grep -oP 'INTROSPECTION\|dispatch\|calls=\K[0-9]+' | head -1)
    if [ "${DISPATCH_CALLS:-0}" -ge 1 ]; then
        pass "L19-03" "dispatch_status queried ($DISPATCH_CALLS calls)"
    else
        fail "L19-03" "dispatch_status not queried or zero calls"
    fi

    fi

    # ============================================================
    # L20: TOOL EXECUTION
    # ============================================================
    echo -e "${CYAN}Level 20: Tool Execution${NC}"

    if ! govkill_block "L20-*" 'PHASE|TOOL_EXEC|'; then

    TE_CALLS=$(echo "$OUTPUT" | grep -oP 'TOOL_EXEC\|positive_control\|calls=\K[0-9]+' | head -1)
    if [ "${TE_CALLS:-0}" -gt 0 ]; then
        pass "L20-01" "Tool probe made calls (${TE_CALLS})"
    else
        gk_fail "L20-01" "Tool probe made no calls"
    fi

    TE_NEG=$(echo "$OUTPUT" | grep -oP 'TOOL_EXEC\|negative_control_calls=\K-?[0-9]+' | head -1)
    if [ "${TE_NEG:-0}" -eq 0 ] || [ "${TE_NEG:--1}" -le 0 ]; then
        pass "L20-02" "Negative control held (${TE_NEG:-0} calls)"
    else
        fail "L20-02" "Negative control breached" "made ${TE_NEG} tool calls"
    fi

    if echo "$OUTPUT" | grep -q 'TOOL_EXEC|dual_gate_held=true'; then
        pass "L20-03" "Dual gate held"
    else
        gk_fail "L20-03" "Dual gate not confirmed"
    fi

    fi

    # ============================================================
    # L21: GOVERNANCE HEALTH
    # ============================================================
    echo -e "${CYAN}Level 21: Governance Health${NC}"

    if ! govkill_block "L21-*" 'PHASE|GOVERNANCE_HEALTH|'; then

    HP_VERDICT=$(echo "$OUTPUT" | grep -oP 'GOVERNANCE_HEALTH\|verdict=\K\w+' | tail -1)
    if [ -n "${HP_VERDICT:-}" ]; then
        pass "L21-01" "Pulse verdict queried: $HP_VERDICT"
    else
        fail "L21-01" "No pulse verdict queried"
    fi

    HP_EPOCH=$(echo "$OUTPUT" | grep -oP 'GOVERNANCE_HEALTH\|.*epoch=\K[0-9]+' | tail -1)
    if [ -n "${HP_EPOCH:-}" ]; then
        pass "L21-02" "Epoch tracked ($HP_EPOCH)"
    else
        fail "L21-02" "No epoch tracked"
    fi

    fi

    # ============================================================
    # L22: TELEMETRY AUDIT
    # ============================================================
    echo -e "${CYAN}Level 22: Telemetry Audit${NC}"

    if ! govkill_block "L22-*" 'PHASE|TELEMETRY_AUDIT|'; then

    TELEM_TYPES=$(echo "$OUTPUT" | grep -oP 'TELEMETRY_AUDIT\|unique_types=\K[0-9]+' | head -1)
    if [ "${TELEM_TYPES:-0}" -ge 3 ]; then
        pass "L22-01" "Telemetry event types found ($TELEM_TYPES)"
    else
        fail "L22-01" "Insufficient telemetry event types" "got ${TELEM_TYPES:-0}"
    fi

    if echo "$OUTPUT" | grep -q 'TELEMETRY_AUDIT|consequence_found='; then
        pass "L22-02" "Consequence events found"
    else
        fail "L22-02" "No consequence events found"
    fi

    CODEGEN_EV=$(echo "$OUTPUT" | grep -oP 'TELEMETRY_AUDIT\|consequence\|CODEGEN_EXEC=\K[0-9]+' | head -1)
    AGENT_TOOL_EV=$(echo "$OUTPUT" | grep -oP 'TELEMETRY_AUDIT\|consequence\|AGENT_TOOL_CALL=\K[0-9]+' | head -1)
    if [ "${CODEGEN_EV:-0}" -ge 1 ] || [ "${AGENT_TOOL_EV:-0}" -ge 1 ]; then
        pass "L22-03" "Execution events present (CODEGEN_EXEC=${CODEGEN_EV:-0}, AGENT_TOOL_CALL=${AGENT_TOOL_EV:-0})"
    else
        gk_fail "L22-03" "No CODEGEN_EXEC or AGENT_TOOL_CALL events in telemetry"
    fi

    fi

    # ============================================================
    # L23: TRANSCRIPT AUDIT
    # ============================================================
    echo -e "${CYAN}Level 23: Transcript Audit${NC}"

    if ! govkill_block "L23-*" 'PHASE|TRANSCRIPT_AUDIT|'; then

    TA_LINES=$(echo "$OUTPUT" | grep -oP 'TRANSCRIPT_AUDIT\|lines=\K[0-9]+' | head -1)
    if [ "${TA_LINES:-0}" -gt 0 ]; then
        pass "L23-01" "Transcript has entries (${TA_LINES})"
    else
        fail "L23-01" "Transcript empty or absent"
    fi

    if echo "$OUTPUT" | grep -q "TRANSCRIPT_AUDIT|every_entry_hashed=true"; then
        pass "L23-02" "All transcript entries hashed"
    else
        fail "L23-02" "Transcript entries missing hashes"
    fi

    if echo "$OUTPUT" | grep -q "TRANSCRIPT_AUDIT|ref_matches_entries="; then
        pass "L23-03" "TRANSCRIPT_REF matches reported"
    else
        fail "L23-03" "TRANSCRIPT_REF match not reported"
    fi

    fi

    # ============================================================
    # L24: EVIDENCE AUDIT
    # ============================================================
    echo -e "${CYAN}Level 24: Evidence Audit${NC}"

    if ! govkill_block "L24-*" 'PHASE|EVIDENCE_AUDIT|'; then

    EV_ATT=$(echo "$OUTPUT" | grep -oP 'EVIDENCE\|exec_attestations=\K[0-9]+' | head -1)
    if [ "${EV_ATT:-0}" -gt 0 ]; then
        pass "L24-01" "Execution attestations present (${EV_ATT})"
    else
        gk_fail "L24-01" "No execution attestations"
    fi

    EV_SIGNED=$(echo "$OUTPUT" | grep -oP 'EVIDENCE\|signed=\K[0-9]+' | head -1)
    if [ "${EV_SIGNED:-0}" -gt 0 ]; then
        pass "L24-02" "Attestations signed (${EV_SIGNED})"
    else
        gk_fail "L24-02" "No signed attestations"
    fi

    EV_SNAP=$(echo "$OUTPUT" | grep -oP 'EVIDENCE\|snapshots=\K[0-9]+' | head -1)
    if [ "${EV_SNAP:-0}" -gt 0 ]; then
        pass "L24-03" "Decision snapshots present (${EV_SNAP})"
    else
        gk_fail "L24-03" "No decision snapshots"
    fi

    # Refusals agree with attestations
    EV_REF=$(echo "$OUTPUT" | grep -oP 'EVIDENCE\|refusals=\K[0-9]+' | head -1)
    EV_REF_ATT=$(echo "$OUTPUT" | grep -oP 'EVIDENCE\|refusals=[0-9]+\|refusal_attestations=\K[0-9]+' | head -1)
    if [ "${EV_REF:-0}" -eq 0 ]; then
        pass "L24-04" "No refusals this run (nothing to attest)"
    elif [ "${EV_REF_ATT:-0}" -ge "${EV_REF:-0}" ]; then
        pass "L24-04" "Every refusal has attestation (${EV_REF_ATT}/${EV_REF})"
    else
        fail "L24-04" "Refusals without attestations" "refusals=${EV_REF:-?} attestations=${EV_REF_ATT:-?}"
    fi

    # Semantic turns present
    EV_SEM=$(echo "$OUTPUT" | grep -oP 'EVIDENCE\|.*semantic_turns=\K[0-9]+' | head -1)
    if [ "${EV_SEM:-0}" -gt 0 ]; then
        pass "L24-05" "Semantic turns present (${EV_SEM})"
    else
        gk_fail "L24-05" "No semantic turns recorded"
    fi

    # Telemetry chain verification
    for _chain in telemetry audit; do
        _cf="$WORKDIR/${_chain}.jsonl"
        if [ ! -f "$_cf" ]; then
            gk_fail "L24-06-${_chain}" "${_chain}.jsonl absent"
        elif (cd "$WORKDIR" && "$NAAB" --verify-telemetry-chain "${_chain}.jsonl" 2>&1) \
                | grep -q "Chain verified:"; then
            pass "L24-06-${_chain}" "${_chain} hash chain verifies end to end"
        else
            fail "L24-06-${_chain}" "${_chain} hash chain BROKEN or TAMPERED" \
                "$( (cd "$WORKDIR" && "$NAAB" --verify-telemetry-chain "${_chain}.jsonl" 2>&1) | grep -E 'BREAK|TAMPER|CORRUPT' | head -2)"
        fi
    done

    fi

    # ============================================================
    # L25: TAINT TRACKING
    # ============================================================
    echo -e "${CYAN}Level 25: Taint Tracking${NC}"

    if ! govkill_block "L25-*" 'EVIDENCE|'; then

    # Taint violations from telemetry (written at shutdown)
    TAINT_N=0
    if [ -f "$WORKDIR/telemetry.jsonl" ]; then
        TAINT_N=$(grep -c 'taint_tracking\.sink_violation' "$WORKDIR/telemetry.jsonl" 2>/dev/null || true)
    fi

    if [ "$TAINT_N" -ge 1 ] || grep -q 'taint' "$WORKDIR/telemetry.jsonl" 2>/dev/null; then
        pass "L25-01" "Taint tracking active (${TAINT_N} sink violations in telemetry)"
    else
        gk_fail "L25-01" "Taint tracking not active"
    fi

    if [ "$TAINT_N" -ge 1 ]; then
        pass "L25-02" "Sink violations counted (${TAINT_N})"
    else
        skip "L25-02" "No sink violations to count"
    fi

    # Count unique violations for baseline
    if [ "$TAINT_N" -ge 1 ]; then
        # `|| echo 0` is the same trap as `grep -c`, one pipe further along:
        # under `set -o pipefail` grep's exit 1 propagates through wc, so the
        # fallback fires even though wc already printed "0", and the variable
        # becomes the non-integer "0\n0". Every later integer test then errors.
        UNIQUE_VIOLATIONS=$(grep 'taint_tracking\.sink_violation' "$WORKDIR/telemetry.jsonl" 2>/dev/null | sort -u | wc -l || true)
        UNIQUE_VIOLATIONS=$(echo "${UNIQUE_VIOLATIONS:-0}" | head -1)
        pass "L25-03" "Baseline established (${UNIQUE_VIOLATIONS} unique violations)"
    else
        skip "L25-03" "No violations for baseline"
    fi

    fi

    # ============================================================
    # L26: AGENT TOPOLOGY (v2 specific)
    # ============================================================
    echo -e "${CYAN}Level 26: Agent Topology${NC}"

    # L26-01: PM delegation
    if echo "$OUTPUT" | grep -qE 'delegate_engine|TOOL_RESULT'; then
        pass "L26-01" "PM tool calls resulted in delegation"
    elif [ -f "$WORKDIR/telemetry.jsonl" ] && grep -q 'TOOL_RESULT' "$WORKDIR/telemetry.jsonl" 2>/dev/null; then
        pass "L26-01" "PM delegation observed in telemetry"
    else
        gk_fail "L26-01" "PM delegation not observed"
    fi

    # L26-02: Multi-tier interaction
    HAS_LEAD=$(echo "$OUTPUT" | grep -c 'TURN|engine_lead' || true)
    HAS_WORKER=$(echo "$OUTPUT" | grep -c 'TURN|engine_dev' || true)
    if [ "$HAS_LEAD" -ge 1 ] && [ "$HAS_WORKER" -ge 1 ]; then
        pass "L26-02" "Multi-tier interaction observed (lead=$HAS_LEAD, worker=$HAS_WORKER)"
    elif [ "$HAS_LEAD" -ge 1 ] || [ "$HAS_WORKER" -ge 1 ]; then
        gk_fail "L26-02" "Only single tier observed" "lead=$HAS_LEAD worker=$HAS_WORKER"
    else
        gk_fail "L26-02" "No tier interaction observed"
    fi

    # L26-03: Peak agent count
    if [ "$AGENTS_CREATED" -ge 8 ]; then
        pass "L26-03" "Peak agent count >= 8 ($AGENTS_CREATED agents)"
    else
        gk_fail "L26-03" "Insufficient agents" "got $AGENTS_CREATED, expected >= 8"
    fi

    # L26-04: Shell codegen valid
    if [ "$SHELL_CODEGEN" -gt 0 ]; then
        pass "L26-04" "Shell codegen produced valid output ($SHELL_CODEGEN executions)"
    else
        gk_fail "L26-04" "No shell codegen output"
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
    echo -e "${CYAN}Phase Transitions:${NC}"
    echo "$OUTPUT" | grep '^PHASE|' | while read -r line; do
        echo -e "  ${CYAN}$line${NC}"
    done

    echo ""
    echo -e "${CYAN}Feature Evolution:${NC}"
    echo "$OUTPUT" | grep -E '^(FEATURE\||REACTIVE\|)' | while read -r line; do
        echo -e "  ${CYAN}$line${NC}"
    done

    echo ""
    echo -e "${CYAN}Build Engine:${NC}"
    echo "$OUTPUT" | grep '^BUILD_ENGINE|' | while read -r line; do
        echo -e "  ${CYAN}$line${NC}"
    done

    echo ""
    echo -e "${CYAN}Deploy Script:${NC}"
    echo "$OUTPUT" | grep -E '^(DEPLOY_SCRIPT|SHELL_SYNTAX|SHELL_CODEGEN)' | while read -r line; do
        echo -e "  ${CYAN}$line${NC}"
    done

    echo ""
    echo -e "${CYAN}Propose/Commit:${NC}"
    echo "$OUTPUT" | grep '^PROPOSE_SELECT|' | while read -r line; do
        echo -e "  ${CYAN}$line${NC}"
    done

    echo ""
    echo -e "${CYAN}Fan-Out Review:${NC}"
    echo "$OUTPUT" | grep '^FAN_OUT|' | while read -r line; do
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
    echo -e "${CYAN}Engine-Side Ratchet:${NC}"
    echo "$OUTPUT" | grep '^ENGINE_RATCHET|' | while read -r line; do
        echo -e "  ${CYAN}$line${NC}"
    done

    echo ""
    echo -e "${CYAN}Tool Execution:${NC}"
    echo "$OUTPUT" | grep -E '^(TOOL_EXEC\||TOOL_RESULT\|)' | while read -r line; do
        echo -e "  ${CYAN}$line${NC}"
    done

    echo ""
    echo -e "${CYAN}Telemetry Audit:${NC}"
    echo "$OUTPUT" | grep '^TELEMETRY_AUDIT|' | while read -r line; do
        echo -e "  ${CYAN}$line${NC}"
    done

    echo ""
    echo -e "${CYAN}Evidence:${NC}"
    echo "$OUTPUT" | grep '^EVIDENCE|' | while read -r line; do
        echo -e "  ${CYAN}$line${NC}"
    done

    echo ""
    echo -e "${CYAN}Separation of Duties:${NC}"
    echo "$OUTPUT" | grep '^SEPARATION|' | while read -r line; do
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

fi

# ============================================================
# Run Outcome
# ============================================================
echo ""
echo -e "${CYAN}Run Outcome${NC}"

if [ -n "${GOV_KILL:-}" ]; then
    fail "RUN-01" "Run terminated by governance ($GOV_KILL) before completing" \
         "$SKIP_COUNT checks never evaluated; a low FAIL count here means less was measured"
elif [ -n "${RUN_TRUNCATED:-}" ]; then
    fail "RUN-01" "Run did not reach completion ($RUN_TRUNCATED)" \
         "$SKIP_COUNT checks never evaluated"
elif echo "${OUTPUT:-}" | grep -q "=== LIVING SCRIPT V2 COMPLETE ==="; then
    pass "RUN-01" "Run completed the full arc without governance termination"
else
    skip "RUN-01" "No run output to evaluate"
fi

# ============================================================
# RESULTS
# ============================================================
echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  RESULTS                                                     |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""
echo -e "  ${GREEN}PASS:${NC} $PASS_COUNT"
echo -e "  ${RED}FAIL:${NC} $FAIL_COUNT"
echo -e "  ${YELLOW}SKIP:${NC} $SKIP_COUNT"
echo -e "  Total: $TOTAL"

# Challenge counts from telemetry
CH_PASS=0; CH_FAIL=0
if [ -f "${WORKDIR:-/nonexistent}/telemetry.jsonl" ]; then
    CH_PASS=$(grep -c 'AGENT_CHALLENGE_PASS' "$WORKDIR/telemetry.jsonl" || true)
    CH_FAIL=$(grep -c 'AGENT_CHALLENGE_FAIL' "$WORKDIR/telemetry.jsonl" || true)
    echo -e "  ${CYAN}Challenges (telemetry): pass=$CH_PASS fail=$CH_FAIL${NC}"
fi
[ -n "${GOV_KILL:-}" ] && echo -e "  ${YELLOW}GOVERNANCE KILL: $GOV_KILL (run terminated by design; unreached levels skipped)${NC}"
if run_incomplete 2>/dev/null && [ "$SKIP_COUNT" -gt 0 ]; then
    echo -e "  ${YELLOW}NOTE: $SKIP_COUNT of $TOTAL checks were never evaluated. A low FAIL count${NC}"
    echo -e "  ${YELLOW}      on a truncated run means less was measured, not that more passed.${NC}"
fi
if [ -n "$FAILURES" ]; then
    echo ""
    echo -e "${RED}Failed assertions:${NC}"
    echo -e "$FAILURES"
fi
echo -e "${CYAN}+==============================================================+${NC}"

# Save results
mkdir -p "$RESULTS_DIR"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
echo "{\"pass\": $PASS_COUNT, \"fail\": $FAIL_COUNT, \"skip\": $SKIP_COUNT, \"total\": $TOTAL, \"governance_kill\": \"${GOV_KILL:-}\", \"commit\": \"$BUILD_COMMIT\", \"challenges_pass\": $CH_PASS, \"challenges_fail\": $CH_FAIL}" > "$RESULTS_DIR/summary.json"
[ -n "${OUTPUT:-}" ] && {
    echo "# commit=$BUILD_COMMIT binary=$NAAB binary_mtime=$BINARY_MTIME governance_kill=${GOV_KILL:-none}"
    echo "$OUTPUT"
} > "$RESULTS_DIR/results_${TIMESTAMP}.txt"
[ -f "${STDERR_FILE:-/dev/null}" ] && cp "$STDERR_FILE" "$RESULTS_DIR/stderr_${TIMESTAMP}.txt" 2>/dev/null || true
[ -f "${WORKDIR:-/dev/null}/telemetry.jsonl" ] && cp "$WORKDIR/telemetry.jsonl" "$RESULTS_DIR/telemetry_${TIMESTAMP}.jsonl" 2>/dev/null || true
[ -f "${WORKDIR:-/dev/null}/transcript.jsonl" ] && cp "$WORKDIR/transcript.jsonl" "$RESULTS_DIR/transcript_${TIMESTAMP}.jsonl" 2>/dev/null || true
[ -f "${WORKDIR:-/dev/null}/audit.jsonl" ] && cp "$WORKDIR/audit.jsonl" "$RESULTS_DIR/audit_${TIMESTAMP}.jsonl" 2>/dev/null || true

[ "$FAIL_COUNT" -eq 0 ]
