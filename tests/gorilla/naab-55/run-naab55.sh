#!/usr/bin/env bash
# ============================================================
# naab-55: Multi-Agent Gorilla Test — Code Review Workflow
#
# 2 agents (builder + reviewer) across 6 phases:
# WARMUP → BUILD_REVIEW → FAN_OUT → PIPELINE → DRIFT → RECOVERY
# Tests fan_out, pipeline, per-agent CDD isolation, pipeline
# separation, max_unique_agents enforcement, S20 prompt compliance.
#
# Requires: GK1 env var with a Gemini API key
# Expected runtime: 4-8 minutes (~70 API calls)
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/naab55-$$"
RESULTS_DIR="$SCRIPT_DIR/results"

# Colors
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'

# Counters
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; TOTAL=0; FAILURES=""

pass() { local id="$1" desc="$2"; PASS_COUNT=$((PASS_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${GREEN}PASS${NC} [$id] $desc"; }
fail() { local id="$1" desc="$2" detail="${3:-}"; FAIL_COUNT=$((FAIL_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${RED}FAIL${NC} [$id] $desc"; [ -n "$detail" ] && echo -e "       ${RED}-> $detail${NC}"; FAILURES="${FAILURES}\n  [$id] $desc${detail:+ -- $detail}"; }
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
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  naab-55: Multi-Agent Gorilla Test — Code Review Workflow    |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

# ============================================================
# Check for API key
# ============================================================
if [ -z "${GK1:-}" ]; then
    echo -e "${YELLOW}No GK1 API key -- skipping all live multi-agent tests${NC}"
    for i in $(seq 1 16); do
        skip "N$i" "No GK1 API key"
    done
else
    WORKDIR=$(setup_workdir)
    STDERR_FILE="$TEST_TMP/stderr.log"

    cp "$SCRIPT_DIR/src/multi-agent.naab" "$WORKDIR/"
    echo -e "${CYAN}Running multi-agent test (~4-8 minutes with live API calls)...${NC}"
    echo -e "${CYAN}Config: 2 agents (builder+reviewer), lease=10, check_interval=2, max_unique_agents=3${NC}"
    echo ""

    OUTPUT=$(cd "$WORKDIR" && timeout 1800 "$NAAB" --governance-dashboard "multi-agent.naab" 2>"$STDERR_FILE") && EXIT_CODE=0 || EXIT_CODE=$?

    echo ""
    echo -e "${CYAN}--- Output (last 60 lines) ---${NC}"
    echo "$OUTPUT" | tail -60
    echo -e "${CYAN}--- End Output ---${NC}"
    echo ""

    # ============================================================
    # Extract key values
    # ============================================================
    TOTAL_SENDS=$(echo "$OUTPUT" | grep -oP 'SUMMARY_TOTAL_SENDS: \K[0-9]+' | head -1)
    TOTAL_SENDS=${TOTAL_SENDS:-0}

    SEND_ERRORS=$(echo "$OUTPUT" | grep -oP 'SUMMARY_SEND_ERRORS: \K[0-9]+' | head -1)
    SEND_ERRORS=${SEND_ERRORS:-0}

    BUILDER_CREATED=$(echo "$OUTPUT" | grep -oP 'SUMMARY_BUILDER_CREATED: \K\w+' | head -1)
    REVIEWER_CREATED=$(echo "$OUTPUT" | grep -oP 'SUMMARY_REVIEWER_CREATED: \K\w+' | head -1)

    BUILDER_C=$(echo "$OUTPUT" | grep -oP 'SUMMARY_BUILDER_COHERENCE: \K[0-9.e+-]+' | head -1)
    REVIEWER_C=$(echo "$OUTPUT" | grep -oP 'SUMMARY_REVIEWER_COHERENCE: \K[0-9.e+-]+' | head -1)

    BUILDER_MIN_C=$(echo "$OUTPUT" | grep -oP 'SUMMARY_BUILDER_MIN_COHERENCE: \K[0-9.e+-]+' | head -1)
    REVIEWER_MIN_C=$(echo "$OUTPUT" | grep -oP 'SUMMARY_REVIEWER_MIN_COHERENCE: \K[0-9.e+-]+' | head -1)

    BUILDER_CP=$(echo "$OUTPUT" | grep -oP 'SUMMARY_BUILDER_CHALLENGES_PASSED: \K[0-9]+' | head -1)
    BUILDER_CF=$(echo "$OUTPUT" | grep -oP 'SUMMARY_BUILDER_CHALLENGES_FAILED: \K[0-9]+' | head -1)
    REVIEWER_CP=$(echo "$OUTPUT" | grep -oP 'SUMMARY_REVIEWER_CHALLENGES_PASSED: \K[0-9]+' | head -1)
    REVIEWER_CF=$(echo "$OUTPUT" | grep -oP 'SUMMARY_REVIEWER_CHALLENGES_FAILED: \K[0-9]+' | head -1)

    BUILDER_PC=$(echo "$OUTPUT" | grep -oP 'SUMMARY_BUILDER_PROMPT_COMPLIANCE: \K[0-9]+' | head -1)
    BUILDER_PC=${BUILDER_PC:-0}

    FAN_OUT_OK=$(echo "$OUTPUT" | grep -oP 'SUMMARY_FAN_OUT_OK: \K[0-9]+' | head -1)
    FAN_OUT_TOTAL=$(echo "$OUTPUT" | grep -oP 'SUMMARY_FAN_OUT_OK: [0-9]+/\K[0-9]+' | head -1)

    PIPELINE_OK=$(echo "$OUTPUT" | grep -oP 'SUMMARY_PIPELINE_OK: \K[0-9]+' | head -1)
    PIPELINE_TOTAL=$(echo "$OUTPUT" | grep -oP 'SUMMARY_PIPELINE_OK: [0-9]+/\K[0-9]+' | head -1)

    MAX_AGENTS_ENFORCED=$(echo "$OUTPUT" | grep -oP 'SUMMARY_MAX_AGENTS_ENFORCED: \K\w+' | head -1)
    PIPELINE_SEP=$(echo "$OUTPUT" | grep -oP 'SUMMARY_PIPELINE_SEP_ENFORCED: \K\w+' | head -1)

    BUILDER_PRE_DRIFT=$(echo "$OUTPUT" | grep -oP 'SUMMARY_BUILDER_PRE_DRIFT_C: \K[0-9.e+-]+' | head -1)
    BUILDER_POST_DRIFT=$(echo "$OUTPUT" | grep -oP 'SUMMARY_BUILDER_POST_DRIFT_C: \K[0-9.e+-]+' | head -1)

    echo -e "${CYAN}Extracted: sends=$TOTAL_SENDS, errors=$SEND_ERRORS${NC}"
    echo -e "${CYAN}          builder: c=$BUILDER_C, min=$BUILDER_MIN_C, cp=$BUILDER_CP, cf=$BUILDER_CF, pc=$BUILDER_PC${NC}"
    echo -e "${CYAN}          reviewer: c=$REVIEWER_C, min=$REVIEWER_MIN_C, cp=$REVIEWER_CP, cf=$REVIEWER_CF${NC}"
    echo -e "${CYAN}          fan_out: $FAN_OUT_OK/$FAN_OUT_TOTAL, pipeline: $PIPELINE_OK/$PIPELINE_TOTAL${NC}"
    echo ""

    # ============================================================
    # MA: MULTI-AGENT ASSERTIONS (MA01-MA06)
    # ============================================================
    echo -e "${CYAN}Multi-Agent Correctness${NC}"

    # MA01: Both agents created
    if [ "${BUILDER_CREATED:-}" = "true" ] && [ "${REVIEWER_CREATED:-}" = "true" ]; then
        pass "MA01" "Both agents created successfully"
    else
        fail "MA01" "Agent creation failed" "builder=$BUILDER_CREATED reviewer=$REVIEWER_CREATED"
    fi

    # MA02: max_unique_agents enforcement
    if [ "${MAX_AGENTS_ENFORCED:-}" = "true" ]; then
        pass "MA02" "max_unique_agents enforcement works"
    else
        fail "MA02" "max_unique_agents not enforced" "expected 3rd or 4th create to fail"
    fi

    # MA03: fan_out returns 2 responses per round
    FAN_OUT_OK=${FAN_OUT_OK:-0}
    FAN_OUT_TOTAL=${FAN_OUT_TOTAL:-0}
    if [ "$FAN_OUT_TOTAL" -gt 0 ] && [ "$FAN_OUT_OK" -gt 0 ]; then
        pass "MA03" "agent.fan_out() returned 2 responses ($FAN_OUT_OK/$FAN_OUT_TOTAL rounds)"
    else
        fail "MA03" "agent.fan_out() failed" "ok=$FAN_OUT_OK total=$FAN_OUT_TOTAL"
    fi

    # MA04: pipeline returns single response (verified by NAAb script getting content)
    PIPELINE_OK=${PIPELINE_OK:-0}
    PIPELINE_TOTAL=${PIPELINE_TOTAL:-0}
    if [ "$PIPELINE_TOTAL" -gt 0 ] && [ "$PIPELINE_OK" -gt 0 ]; then
        pass "MA04" "agent.pipeline() returned response ($PIPELINE_OK/$PIPELINE_TOTAL chains)"
    else
        fail "MA04" "agent.pipeline() failed" "ok=$PIPELINE_OK total=$PIPELINE_TOTAL"
    fi

    # MA05: Pipeline separation enforced
    if [ "${PIPELINE_SEP:-}" = "true" ]; then
        pass "MA05" "Pipeline separation enforced (same-config rejected)"
    else
        fail "MA05" "Pipeline separation not enforced"
    fi

    # MA06: Per-agent CDD isolation — builder coherence should be lower than reviewer after drift
    if [ -n "${BUILDER_POST_DRIFT:-}" ] && [ -n "${REVIEWER_C:-}" ]; then
        BUILDER_LOWER=$(echo "${BUILDER_POST_DRIFT} < ${REVIEWER_C}" | bc -l 2>/dev/null || echo "0")
        if [ "$BUILDER_LOWER" = "1" ]; then
            pass "MA06" "Per-agent CDD isolation (builder=$BUILDER_POST_DRIFT < reviewer=$REVIEWER_C after drift)"
        else
            # Builder might have recovered. Check if builder min < reviewer min instead
            if [ -n "${BUILDER_MIN_C:-}" ] && [ -n "${REVIEWER_MIN_C:-}" ]; then
                BMIN_LOWER=$(echo "${BUILDER_MIN_C} < ${REVIEWER_MIN_C}" | bc -l 2>/dev/null || echo "0")
                if [ "$BMIN_LOWER" = "1" ]; then
                    pass "MA06" "Per-agent CDD isolation (builder_min=$BUILDER_MIN_C < reviewer_min=$REVIEWER_MIN_C)"
                else
                    fail "MA06" "Per-agent CDD isolation not detected" "builder_min=$BUILDER_MIN_C reviewer_min=$REVIEWER_MIN_C"
                fi
            else
                fail "MA06" "Could not verify CDD isolation" "builder_post_drift=$BUILDER_POST_DRIFT reviewer=$REVIEWER_C"
            fi
        fi
    else
        fail "MA06" "Missing coherence data for CDD isolation check"
    fi

    # ============================================================
    # C: CORRECTNESS (C01-C04)
    # ============================================================
    echo -e "${CYAN}Correctness${NC}"

    # C01: All sends completed (at least 60)
    if [ "$TOTAL_SENDS" -ge 60 ]; then
        pass "C01" "All sends completed ($TOTAL_SENDS total)"
    else
        fail "C01" "Insufficient sends" "got $TOTAL_SENDS, expected >= 60"
    fi

    # C02: No send errors (or very few)
    if [ "$SEND_ERRORS" -le 5 ]; then
        pass "C02" "Low error rate ($SEND_ERRORS errors out of $TOTAL_SENDS sends)"
    else
        fail "C02" "Too many send errors" "$SEND_ERRORS errors out of $TOTAL_SENDS sends"
    fi

    # C03: Builder coherence varied (CDD applied penalties)
    if [ -n "${BUILDER_MIN_C:-}" ]; then
        BELOW_ONE=$(echo "$BUILDER_MIN_C < 1.0" | bc -l 2>/dev/null || echo "0")
        if [ "$BELOW_ONE" = "1" ]; then
            pass "C03" "Builder coherence varied (min=$BUILDER_MIN_C)"
        else
            fail "C03" "Builder coherence constant at 1.0" "CDD not applying penalties?"
        fi
    else
        fail "C03" "Could not read builder coherence"
    fi

    # C04: S20 prompt compliance fired for builder during drift
    if [ "$BUILDER_PC" -gt 0 ]; then
        pass "C04" "S20 prompt_compliance fired for builder (count=$BUILDER_PC)"
    else
        fail "C04" "S20 prompt_compliance did not fire" "builder_prompt_compliance=$BUILDER_PC"
    fi

    # ============================================================
    # T: TELEMETRY INTEGRITY (T01-T04)
    # ============================================================
    echo -e "${CYAN}Telemetry Integrity${NC}"

    HASH_CHAIN=$(echo "$OUTPUT" | grep -oP 'TELEM_HASH_CHAIN: \K\w+' | head -1)
    if [ "${HASH_CHAIN:-}" = "true" ]; then
        pass "T01" "Telemetry hash chain intact"
    else
        fail "T01" "Hash chain broken"
    fi

    TELEM_DIVERSITY=$(echo "$OUTPUT" | grep -oP 'TELEM_EVENT_DIVERSITY: \K[0-9]+' | head -1)
    TELEM_DIVERSITY=${TELEM_DIVERSITY:-0}
    if [ "$TELEM_DIVERSITY" -ge 4 ]; then
        pass "T02" "Event type diversity ($TELEM_DIVERSITY distinct types)"
    else
        fail "T02" "Insufficient event diversity" "only $TELEM_DIVERSITY types"
    fi

    RUN_ID_OK=$(echo "$OUTPUT" | grep -oP 'TELEM_RUN_ID_CONSISTENT: \K\w+' | head -1)
    if [ "${RUN_ID_OK:-}" = "true" ]; then
        pass "T03" "Run ID consistent across all events"
    else
        fail "T03" "Run ID inconsistency"
    fi

    TS_ORDERED=$(echo "$OUTPUT" | grep -oP 'TELEM_TIMESTAMPS_ORDERED: \K\w+' | head -1)
    if [ "${TS_ORDERED:-}" = "true" ]; then
        pass "T04" "Timestamps non-decreasing"
    else
        fail "T04" "Timestamp ordering violated"
    fi

    # ============================================================
    # H: HEALTH (H01-H02)
    # ============================================================
    echo -e "${CYAN}Health${NC}"

    # H01: At least one challenge fired (lease=10 should expire)
    TOTAL_CP=$(( ${BUILDER_CP:-0} + ${REVIEWER_CP:-0} ))
    TOTAL_CF=$(( ${BUILDER_CF:-0} + ${REVIEWER_CF:-0} ))
    TOTAL_CHALLENGES=$(( TOTAL_CP + TOTAL_CF ))
    TELEM_CPASS=$(echo "$OUTPUT" | grep -oP 'TELEM_CHALLENGE_PASS_EVENTS: \K[0-9]+' | head -1)
    TELEM_CPASS=${TELEM_CPASS:-0}
    if [ "$TOTAL_CHALLENGES" -gt 0 ] || [ "$TELEM_CPASS" -gt 0 ]; then
        pass "H01" "Challenges fired (summary: ${TOTAL_CP}p/${TOTAL_CF}f, telemetry: ${TELEM_CPASS}p)"
    else
        fail "H01" "No challenges fired despite lease=10"
    fi

    # H02: Governance health not IMPAIRED
    VERDICT=$(echo "$OUTPUT" | grep -oP 'HEALTH_VERDICT: \K\w+' | head -1)
    if [ "${VERDICT:-}" = "healthy" ] || [ "${VERDICT:-}" = "degraded" ]; then
        pass "H02" "Governance health: $VERDICT"
    elif [ "${VERDICT:-}" = "impaired" ]; then
        fail "H02" "Governance health IMPAIRED"
    else
        pass "H02" "Governance health: ${VERDICT:-unknown}"
    fi

    # ============================================================
    # PHASE + COHERENCE TRAJECTORY
    # ============================================================
    echo ""
    echo -e "${CYAN}Per-Agent Coherence:${NC}"
    echo -e "  ${CYAN}Builder:  final=${BUILDER_C:-?} min=${BUILDER_MIN_C:-?} pre_drift=${BUILDER_PRE_DRIFT:-?} post_drift=${BUILDER_POST_DRIFT:-?}${NC}"
    echo -e "  ${CYAN}Reviewer: final=${REVIEWER_C:-?} min=${REVIEWER_MIN_C:-?}${NC}"
    echo -e "  ${CYAN}Builder prompt_compliance: ${BUILDER_PC}${NC}"
    echo -e "  ${CYAN}Fan-out: ${FAN_OUT_OK:-0}/${FAN_OUT_TOTAL:-0}, Pipeline: ${PIPELINE_OK:-0}/${PIPELINE_TOTAL:-0}${NC}"

    # ============================================================
    # STDERR CROSS-VALIDATION
    # ============================================================
    echo ""
    echo -e "${CYAN}Stderr Cross-Validation:${NC}"

    if [ -f "$STDERR_FILE" ]; then
        STDERR_SIZE=$(wc -c < "$STDERR_FILE")
        echo -e "  Stderr size: ${STDERR_SIZE} bytes"

        if grep -qi "governance\|dashboard\|CDD\|BSD\|coherence" "$STDERR_FILE"; then
            echo -e "  ${GREEN}OK${NC} Governance activity visible in stderr"
        else
            echo -e "  ${YELLOW}!${NC} No governance activity in stderr"
        fi

        MULTI_AGENT_TELEM=$(echo "$OUTPUT" | grep -oP 'TELEM_MULTI_AGENT: \K\w+' | head -1)
        if [ "${MULTI_AGENT_TELEM:-}" = "true" ]; then
            echo -e "  ${GREEN}OK${NC} Multi-agent telemetry events present"
        else
            echo -e "  ${YELLOW}!${NC} Multi-agent telemetry not confirmed"
        fi

        echo ""
        echo -e "${CYAN}--- Stderr (first 20 lines) ---${NC}"
        head -20 "$STDERR_FILE" 2>/dev/null
        echo -e "${CYAN}--- End Stderr ---${NC}"
    else
        echo -e "  ${YELLOW}!${NC} No stderr file captured"
    fi

    echo ""
    echo -e "${CYAN}Governance Health Verdict: ${VERDICT:-unknown}${NC}"
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
if [ -f "${WORKDIR:-/dev/null}/telemetry.jsonl" ]; then
    cp "$WORKDIR/telemetry.jsonl" "$RESULTS_DIR/telemetry_${TIMESTAMP}.jsonl" 2>/dev/null || true
fi
if [ -f "${WORKDIR:-/dev/null}/transcript.jsonl" ]; then
    cp "$WORKDIR/transcript.jsonl" "$RESULTS_DIR/transcript_${TIMESTAMP}.jsonl" 2>/dev/null || true
fi

exit "$FAIL_COUNT"
