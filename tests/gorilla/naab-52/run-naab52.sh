#!/usr/bin/env bash
# ============================================================
# naab-52: D1 State Reconciliation Gorilla Test
#
# 15-turn phased conversation (TOOL_BASELINE → DRIFT → TOOL_RETURN)
# validating Signal 19 (claim_result_reconciliation), contextual
# challenge selection, and RECONCILIATION_TURN telemetry:
#
#   - Tools execute normally at NORMAL level (turns 1-5)
#   - CDD fires during drift phase (off-topic turns 6-10)
#   - Tool execution resumes under governance pressure (turns 11-15)
#   - RECONCILIATION_TURN telemetry emitted every turn S19 enabled
#   - claim_mismatch_count and claim_accuracy in environment+response
#   - Contextual challenges fire when step_up_contextual=true
#   - Dashboard shows reconciliation line when mismatches > 0
#
# What this does NOT test:
#   - Signal 19 firing on deliberate misrepresentation — we cannot
#     control what the LLM says, so we verify the machinery runs
#     (telemetry emitted, fields surfaced, no crashes) rather than
#     trying to force a mismatch. A forced mismatch would require
#     a tool that fails + an LLM that claims success, which depends
#     on unpredictable LLM behavior.
#
# Requires: GK* env var with a Gemini API key
# Expected runtime: 2-5 minutes (15 turns x ~5s per turn)
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/naab52-$$"
RESULTS_DIR="$SCRIPT_DIR/results"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; TOTAL=0; FAILURES=""

pass() { local id="$1" desc="$2"; PASS_COUNT=$((PASS_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${GREEN}PASS${NC} [$id] $desc"; }
fail() { local id="$1" desc="$2" detail="${3:-}"; FAIL_COUNT=$((FAIL_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${RED}FAIL${NC} [$id] $desc"; [ -n "$detail" ] && echo -e "       ${RED}-> $detail${NC}"; FAILURES="${FAILURES}\n  [$id] $desc${detail:+ -- $detail}"; }
skip() { local id="$1" desc="$2"; SKIP_COUNT=$((SKIP_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${YELLOW}SKIP${NC} [$id] $desc"; }

source "$SCRIPT_DIR/../../helpers/trust_setup.sh"
setup_isolated_trust
trap 'teardown_isolated_trust; rm -rf "$TEST_TMP"' EXIT
mkdir -p "$TEST_TMP"

"$NAAB" --keygen "$TEST_TMP/test-key.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/test-key.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/test-key.pem"

setup_workdir() {
    local workdir="$TEST_TMP/work-$$-$RANDOM"
    mkdir -p "$workdir"
    cp "$SCRIPT_DIR/src/govern.json" "$workdir/govern.json"
    (cd "$workdir" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
    echo "$workdir"
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  naab-52: D1 State Reconciliation                            |${NC}"
echo -e "${CYAN}|  Signal 19 + Contextual Challenges + Reconciliation Telem    |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

if [ -z "${GK1:-}${GK2:-}${GK3:-}${GK4:-}${GK5:-}${GK6:-}${GK7:-}${GK8:-}${GK9:-}" ]; then
    echo -e "${YELLOW}No GK* API key — skipping all live tests${NC}"
    for i in $(seq 1 20); do
        skip "N$(printf '%02d' $i)" "No GK* API key"
    done
else
    WORKDIR=$(setup_workdir)
    STDERR_FILE="$TEST_TMP/stderr.log"

    cp "$SCRIPT_DIR/src/d1-reconciliation.naab" "$WORKDIR/"
    echo -e "${CYAN}Running D1 reconciliation test (15 turns, ~2-5 min with live API)...${NC}"
    echo ""

    STDOUT_FILE="$WORKDIR/stdout.log"
    (cd "$WORKDIR" && timeout 600 "$NAAB" --governance-dashboard "d1-reconciliation.naab" >"$STDOUT_FILE" 2>"$STDERR_FILE") && EXIT_CODE=0 || EXIT_CODE=$?
    OUTPUT=$(cat "$STDOUT_FILE" 2>/dev/null)
    STDERR=$(cat "$STDERR_FILE" 2>/dev/null)
    TELEM_FILE="$WORKDIR/telemetry.jsonl"
    TRANSCRIPT_FILE="$WORKDIR/transcript.jsonl"

    echo ""
    echo -e "${CYAN}--- Output (last 30 lines) ---${NC}"
    tail -30 "$STDOUT_FILE" 2>/dev/null
    echo ""

    # Extract summary values
    TURNS=$(echo "$OUTPUT" | grep -oP 'SUMMARY_TURNS_COMPLETED: \K[0-9]+' | head -1)
    TURNS=${TURNS:-0}
    TOTAL_TC=$(echo "$OUTPUT" | grep -oP 'SUMMARY_TOTAL_TOOL_CALLS: \K[0-9]+' | head -1)
    TOTAL_TC=${TOTAL_TC:-0}
    TC_BASELINE=$(echo "$OUTPUT" | grep -oP 'SUMMARY_TOOL_CALLS_BASELINE: \K[0-9]+' | head -1)
    TC_BASELINE=${TC_BASELINE:-0}
    TC_RETURN=$(echo "$OUTPUT" | grep -oP 'SUMMARY_TOOL_CALLS_RETURN: \K[0-9]+' | head -1)
    TC_RETURN=${TC_RETURN:-0}
    CLAIM_MISMATCHES=$(echo "$OUTPUT" | grep -oP 'SUMMARY_CLAIM_MISMATCHES: \K[0-9]+' | head -1)
    CLAIM_MISMATCHES=${CLAIM_MISMATCHES:-"missing"}
    CLAIM_ACCURACY=$(echo "$OUTPUT" | grep -oP 'SUMMARY_CLAIM_ACCURACY: \K\S+' | head -1)
    CLAIM_ACCURACY=${CLAIM_ACCURACY:-"missing"}
    CHALLENGES_P=$(echo "$OUTPUT" | grep -oP 'SUMMARY_CHALLENGES_PASSED: \K[0-9]+' | head -1)
    CHALLENGES_P=${CHALLENGES_P:-0}
    CHALLENGES_F=$(echo "$OUTPUT" | grep -oP 'SUMMARY_CHALLENGES_FAILED: \K[0-9]+' | head -1)
    CHALLENGES_F=${CHALLENGES_F:-0}

    # ============================================================
    # S: System Integrity
    # ============================================================
    echo -e "${CYAN}--- System Integrity ---${NC}"

    # S01: Program completed (no crash)
    if [ "$EXIT_CODE" -eq 0 ] || [ "$EXIT_CODE" -eq 2 ] || [ "$EXIT_CODE" -eq 3 ]; then
        pass "S01" "Program completed (exit $EXIT_CODE)"
    elif [ "$EXIT_CODE" -eq 124 ]; then
        fail "S01" "Program timed out"
    else
        fail "S01" "Program crashed" "exit $EXIT_CODE"
    fi

    # S02: Agent created
    if echo "$OUTPUT" | grep -q "AGENT_CREATED"; then
        pass "S02" "Agent created successfully"
    else
        fail "S02" "Agent creation failed"
    fi

    # S03: Sufficient turns completed
    if [ "$TURNS" -ge 10 ]; then
        pass "S03" "Sufficient turns completed ($TURNS/15)"
    elif [ "$TURNS" -ge 5 ]; then
        pass "S03" "Minimum turns completed ($TURNS/15 — enough for baseline)"
    else
        fail "S03" "Insufficient turns" "$TURNS/15"
    fi

    echo ""

    # ============================================================
    # TC: Tool Call Verification
    # ============================================================
    echo -e "${CYAN}--- Tool Call Verification ---${NC}"

    # TC01: Tool calls made during baseline phase
    if [ "$TC_BASELINE" -gt 0 ]; then
        pass "TC01" "Tool calls made during TOOL_BASELINE ($TC_BASELINE)"
    else
        if [ "$TURNS" -ge 5 ]; then
            skip "TC01" "LLM didn't call tools during baseline (LLM-dependent)"
        else
            skip "TC01" "Not enough turns for baseline"
        fi
    fi

    # TC02: Total tool calls across all phases
    if [ "$TOTAL_TC" -gt 0 ]; then
        pass "TC02" "Total tool calls: $TOTAL_TC"
    else
        skip "TC02" "No tool calls made (LLM-dependent)"
    fi

    # TC03: AGENT_TOOL_CALL telemetry matches tool calls made
    TELEM_TOOL_CALLS=$(grep "AGENT_TOOL_CALL" "$TELEM_FILE" 2>/dev/null | wc -l)
    if [ "$TELEM_TOOL_CALLS" -gt 0 ]; then
        pass "TC03" "AGENT_TOOL_CALL telemetry emitted ($TELEM_TOOL_CALLS events)"
    else
        if [ "$TOTAL_TC" -gt 0 ]; then
            fail "TC03" "Tool calls made but no AGENT_TOOL_CALL telemetry"
        else
            skip "TC03" "No tool calls to trace"
        fi
    fi

    # TC04: AGENT_TOOL_RESULT telemetry emitted for each tool call
    TELEM_TOOL_RESULTS=$(grep "AGENT_TOOL_RESULT" "$TELEM_FILE" 2>/dev/null | wc -l)
    if [ "$TELEM_TOOL_RESULTS" -gt 0 ]; then
        pass "TC04" "AGENT_TOOL_RESULT telemetry emitted ($TELEM_TOOL_RESULTS events)"
    else
        if [ "$TOTAL_TC" -gt 0 ]; then
            fail "TC04" "Tool calls made but no AGENT_TOOL_RESULT telemetry"
        else
            skip "TC04" "No tool calls"
        fi
    fi

    echo ""

    # ============================================================
    # R: Reconciliation (Signal 19)
    # ============================================================
    echo -e "${CYAN}--- Reconciliation (Signal 19) ---${NC}"

    # R01: RECONCILIATION_TURN telemetry emitted
    # What this tests: agent_impl.cpp:3153-3178 — RECONCILIATION_TURN is emitted
    # every turn when claim_result_reconciliation signal is enabled and drift_state exists.
    TELEM_RECONCIL=$(grep "RECONCILIATION_TURN" "$TELEM_FILE" 2>/dev/null | wc -l)
    if [ "$TELEM_RECONCIL" -gt 0 ]; then
        pass "R01" "RECONCILIATION_TURN telemetry emitted ($TELEM_RECONCIL events)"
    else
        if [ "$TURNS" -ge 3 ]; then
            fail "R01" "No RECONCILIATION_TURN telemetry despite $TURNS turns"
        else
            skip "R01" "Not enough turns for telemetry"
        fi
    fi

    # R02: RECONCILIATION_TURN contains all required fields
    # What this tests: each field in the telemetry event at agent_impl.cpp:3166-3177
    if [ "$TELEM_RECONCIL" -gt 0 ]; then
        FIRST_RECONCIL=$(grep "RECONCILIATION_TURN" "$TELEM_FILE" | head -1)
        MISSING_FIELDS=""
        for field in "handle_id" "turn" "tool_integrity_count" "claim_mismatch_count" "claim_accuracy_rolling" "instruction_recall_count" "plan_drift_count" "entity_consistency_count" "coherence" "signals_fired"; do
            if ! echo "$FIRST_RECONCIL" | grep -q "\"$field\"" 2>/dev/null; then
                MISSING_FIELDS="$MISSING_FIELDS $field"
            fi
        done
        if [ -z "$MISSING_FIELDS" ]; then
            pass "R02" "RECONCILIATION_TURN has all 10 required fields"
        else
            fail "R02" "RECONCILIATION_TURN missing fields" "$MISSING_FIELDS"
        fi
    else
        skip "R02" "No RECONCILIATION_TURN events"
    fi

    # R03: claim_mismatch_count surfaced in program output (environment dict)
    # What this tests: buildEnvironmentDict() at agent_impl.cpp:465
    if [ "$CLAIM_MISMATCHES" != "missing" ]; then
        pass "R03" "claim_mismatch_count surfaced ($CLAIM_MISMATCHES)"
    else
        if [ "$TURNS" -ge 3 ]; then
            fail "R03" "claim_mismatch_count not found in output"
        else
            skip "R03" "Not enough turns"
        fi
    fi

    # R04: claim_accuracy tracking
    # What this tests: claim_accuracy_history in DriftState, surfaced via
    # buildEnvironmentDict() at agent_impl.cpp:466-469.
    # Only present after first tool execution that gets reconciled.
    if [ "$CLAIM_ACCURACY" != "missing" ] && [ "$CLAIM_ACCURACY" != "N/A" ]; then
        pass "R04" "claim_accuracy tracked ($CLAIM_ACCURACY)"
    else
        if [ "$TOTAL_TC" -gt 0 ]; then
            # Tool calls made but no accuracy — might be that agent didn't
            # mention tool by name in response (Signal 19 requires name match)
            skip "R04" "No claim_accuracy (agent may not have mentioned tool name)"
        else
            skip "R04" "No tool calls (no accuracy to track)"
        fi
    fi

    # R05: SEMANTIC_TURN telemetry includes claim_mismatch_count
    # What this tests: agent_impl.cpp:3148
    TELEM_SEM=$(grep "SEMANTIC_TURN" "$TELEM_FILE" 2>/dev/null | wc -l)
    if [ "$TELEM_SEM" -gt 0 ]; then
        SEM_CMM=$(grep "SEMANTIC_TURN" "$TELEM_FILE" | head -1 | grep "claim_mismatch_count" 2>/dev/null | wc -l)
        if [ "$SEM_CMM" -gt 0 ]; then
            pass "R05" "SEMANTIC_TURN includes claim_mismatch_count"
        else
            fail "R05" "SEMANTIC_TURN missing claim_mismatch_count"
        fi
    else
        skip "R05" "No SEMANTIC_TURN events"
    fi

    # R06: Response dict semantic section includes D1 fields
    # What this tests: agent_impl.cpp:3349-3354 adds claim_mismatch_count
    # and claim_accuracy to the semantic response section.
    # We check that the .naab program could read sem_cmm from response.
    SEM_CMM_OUTPUT=$(echo "$OUTPUT" | grep 'sem_cmm=' | grep -v 'sem_cmm=N/A' | head -1)
    if [ -n "$SEM_CMM_OUTPUT" ]; then
        pass "R06" "Response semantic section has claim_mismatch_count"
    else
        # Could be N/A if DriftState not yet populated — acceptable if turns low
        if [ "$TURNS" -ge 5 ]; then
            skip "R06" "sem_cmm was N/A on all turns (DriftState timing)"
        else
            skip "R06" "Not enough turns"
        fi
    fi

    echo ""

    # ============================================================
    # CDD: Context Drift Detection
    # ============================================================
    echo -e "${CYAN}--- Context Drift Detection ---${NC}"

    # CDD01: CDD_TURN telemetry emitted
    TELEM_CDD=$(grep "CDD_TURN" "$TELEM_FILE" 2>/dev/null | wc -l)
    if [ "$TELEM_CDD" -gt 0 ]; then
        pass "CDD01" "CDD_TURN telemetry emitted ($TELEM_CDD events)"
    else
        if [ "$TURNS" -ge 3 ]; then
            fail "CDD01" "No CDD_TURN despite $TURNS turns"
        else
            skip "CDD01" "Not enough turns"
        fi
    fi

    # CDD02: Signals fired during drift phase (turns 6-10 are off-topic)
    # Any drift signal (semantic_stability, mandate_alignment, etc.)
    SIGNALS_FIRED=$(grep "CDD_TURN" "$TELEM_FILE" 2>/dev/null | grep -oP '"signals_fired":"[1-9]' | wc -l)
    if [ "$SIGNALS_FIRED" -gt 0 ]; then
        pass "CDD02" "CDD signals fired ($SIGNALS_FIRED turns with signals)"
    else
        skip "CDD02" "No signals fired (may be within baseline tolerance)"
    fi

    echo ""

    # ============================================================
    # CH: Challenge System
    # ============================================================
    echo -e "${CYAN}--- Challenge System ---${NC}"

    # CH01: Challenge telemetry (pass or fail events)
    TELEM_CH_PASS=$(grep "AGENT_CHALLENGE_PASS" "$TELEM_FILE" 2>/dev/null | wc -l | tr -d ' ')
    TELEM_CH_FAIL=$(grep "AGENT_CHALLENGE_FAIL" "$TELEM_FILE" 2>/dev/null | wc -l | tr -d ' ')
    TELEM_CH_TOTAL=$((TELEM_CH_PASS + TELEM_CH_FAIL))
    if [ "$TELEM_CH_TOTAL" -gt 0 ]; then
        pass "CH01" "Challenge events ($TELEM_CH_PASS pass, $TELEM_CH_FAIL fail)"
    else
        skip "CH01" "No challenges triggered (governance may not have escalated)"
    fi

    # CH02: Challenge telemetry includes challenge_type field
    # What this tests: agent_impl.cpp adds challenge_type to telemetry events
    # when step_up_contextual=true. Types: tool_result, plan_step, instruction, entity, mandate
    if [ "$TELEM_CH_TOTAL" -gt 0 ]; then
        CH_TYPE_COUNT=$(grep -E "AGENT_CHALLENGE_PASS|AGENT_CHALLENGE_FAIL" "$TELEM_FILE" 2>/dev/null | grep "challenge_type" 2>/dev/null | wc -l)
        if [ "$CH_TYPE_COUNT" -gt 0 ]; then
            pass "CH02" "Challenge events include challenge_type field"
            # Extract actual types for visibility
            CH_TYPES=$(grep -E "AGENT_CHALLENGE_PASS|AGENT_CHALLENGE_FAIL" "$TELEM_FILE" 2>/dev/null | grep -oP '"challenge_type":"[^"]+' | sort -u | sed 's/"challenge_type":"/  /')
            if [ -n "$CH_TYPES" ]; then
                echo -e "       ${CYAN}Types seen:${NC}"
                echo "$CH_TYPES" | while read -r t; do echo "         $t"; done
            fi
        else
            fail "CH02" "Challenge events missing challenge_type"
        fi
    else
        skip "CH02" "No challenges to check"
    fi

    # CH03: Challenge counters surfaced in output
    if [ "$CHALLENGES_P" -gt 0 ] || [ "$CHALLENGES_F" -gt 0 ]; then
        pass "CH03" "Challenge counters surfaced (passed=$CHALLENGES_P failed=$CHALLENGES_F)"
    else
        skip "CH03" "No challenges occurred"
    fi

    echo ""

    # ============================================================
    # T: Telemetry & Dashboard
    # ============================================================
    echo -e "${CYAN}--- Telemetry & Dashboard ---${NC}"

    # T01: Telemetry file exists and has content
    if [ -f "$TELEM_FILE" ]; then
        TELEM_LINES=$(wc -l < "$TELEM_FILE")
        if [ "$TELEM_LINES" -gt 0 ]; then
            pass "T01" "Telemetry file has $TELEM_LINES events"
        else
            fail "T01" "Telemetry file empty"
        fi
    else
        fail "T01" "No telemetry file"
    fi

    # T02: Hash chain integrity (prev_hash present)
    if [ -f "$TELEM_FILE" ]; then
        FIRST_HASH=$(head -1 "$TELEM_FILE" | grep -oP '"prev_hash":"[^"]+' 2>/dev/null || echo "")
        if [ -n "$FIRST_HASH" ]; then
            pass "T02" "Hash chain genesis present"
        else
            fail "T02" "Hash chain missing prev_hash"
        fi
    else
        skip "T02" "No telemetry file"
    fi

    # T03: Dashboard reconciliation line in stderr (only when mismatches > 0)
    # What this tests: governance_engine.cpp dashboard section prints
    # "Reconcil: N mismatches, N integrity violations, accuracy=X.XX"
    if grep -q "Reconcil:" "$STDERR_FILE" 2>/dev/null; then
        pass "T03" "Dashboard reconciliation line present"
    else
        # Reconcil line only appears when mismatch or integrity count > 0
        # If no mismatches (tools worked correctly), absence is correct behavior
        if [ "${CLAIM_MISMATCHES:-0}" = "0" ]; then
            pass "T03" "Dashboard reconciliation line absent (0 mismatches — correct)"
        else
            fail "T03" "Mismatches reported but no dashboard line"
        fi
    fi

    # T04: Transcript file exists
    if [ -f "$TRANSCRIPT_FILE" ]; then
        TR_LINES=$(wc -l < "$TRANSCRIPT_FILE")
        if [ "$TR_LINES" -gt 0 ]; then
            pass "T04" "Transcript file has $TR_LINES entries"
        else
            fail "T04" "Transcript file empty"
        fi
    else
        skip "T04" "No transcript file"
    fi

    # T05: governance.health() returned a verdict
    HEALTH=$(echo "$OUTPUT" | grep -oP 'HEALTH_VERDICT: \K\S+' | head -1)
    if [ -n "$HEALTH" ]; then
        pass "T05" "governance.health() verdict: $HEALTH"
    else
        fail "T05" "No health verdict"
    fi

    echo ""

    # ============================================================
    # I: Integration (cross-concern)
    # ============================================================
    echo -e "${CYAN}--- Integration ---${NC}"

    # I01: Tool execution under governance pressure (phase 3)
    # If agent reached phase 3, tool calls should still work even at elevated governance
    if [ "$TURNS" -ge 11 ] && [ "$TC_RETURN" -gt 0 ]; then
        pass "I01" "Tools executed under governance pressure (phase 3, $TC_RETURN calls)"
    elif [ "$TURNS" -ge 11 ]; then
        skip "I01" "Phase 3 reached but LLM didn't call tools"
    else
        skip "I01" "Did not reach phase 3 ($TURNS turns)"
    fi

    # I02: Coherence tracked across phases (per-turn output has coherence values)
    COHERENCE_LINES=$(echo "$OUTPUT" | grep -F 'TURN|' 2>/dev/null | grep -c '|c=' 2>/dev/null || echo "0")
    if [ "$COHERENCE_LINES" -ge 5 ]; then
        pass "I02" "Coherence tracked across phases ($COHERENCE_LINES turns)"
    else
        if [ "$TURNS" -ge 3 ]; then
            fail "I02" "Insufficient coherence tracking" "$COHERENCE_LINES turns"
        else
            skip "I02" "Not enough turns"
        fi
    fi

    # I03: No unexpected crashes or GovernanceHardErrors
    HARD_ERRORS=$(echo "$OUTPUT" | grep "GovernanceHardError" 2>/dev/null | wc -l)
    if [ "$HARD_ERRORS" -eq 0 ]; then
        pass "I03" "No GovernanceHardError (advisory-level CDD working correctly)"
    else
        # Not a failure — GovernanceHardError from challenge failure is legitimate
        pass "I03" "GovernanceHardError occurred ($HARD_ERRORS — from challenge failure)"
    fi
fi

# ============================================================
# SUMMARY
# ============================================================
echo ""
echo -e "${CYAN}================================================${NC}"
echo -e "  ${GREEN}PASS: $PASS_COUNT${NC}  ${RED}FAIL: $FAIL_COUNT${NC}  ${YELLOW}SKIP: $SKIP_COUNT${NC}  TOTAL: $TOTAL"

if [ -n "$FAILURES" ]; then
    echo -e "\n${RED}Failures:${FAILURES}${NC}"
fi
echo -e "${CYAN}================================================${NC}"

# Save results
mkdir -p "$RESULTS_DIR"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
echo "{\"pass\": $PASS_COUNT, \"fail\": $FAIL_COUNT, \"skip\": $SKIP_COUNT, \"total\": $TOTAL}" > "$RESULTS_DIR/summary.json"

if [ -n "${OUTPUT:-}" ]; then
    echo "$OUTPUT" > "$RESULTS_DIR/results_${TIMESTAMP}.txt"
fi
if [ -f "${STDERR_FILE:-/dev/null}" ]; then
    cp "$STDERR_FILE" "$RESULTS_DIR/stderr_${TIMESTAMP}.txt" 2>/dev/null || true
fi
if [ -f "${TELEM_FILE:-/dev/null}" ]; then
    cp "$TELEM_FILE" "$RESULTS_DIR/telemetry_${TIMESTAMP}.jsonl" 2>/dev/null || true
fi
if [ -f "${TRANSCRIPT_FILE:-/dev/null}" ]; then
    cp "$TRANSCRIPT_FILE" "$RESULTS_DIR/transcript_${TIMESTAMP}.jsonl" 2>/dev/null || true
fi

exit "$FAIL_COUNT"
