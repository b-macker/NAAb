#!/usr/bin/env bash
# ============================================================
# naab-56: 20-Agent Org Simulation
#
# Full software company (20 roles) building a todo app.
# Tests: mass agent creation, fan-out to 20, 6-stage pipeline,
# per-agent CDD isolation, consensus voting, S20 prompt compliance.
#
# Requires: GK1 env var with a Gemini API key
# Expected runtime: 8-15 minutes (~100+ API calls)
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/naab56-$$"
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
echo -e "${CYAN}|  naab-56: 20-Agent Org Simulation — All-In Scaling Test      |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

if [ -z "${GK1:-}" ]; then
    echo -e "${YELLOW}No GK1 API key -- skipping all live tests${NC}"
    for i in $(seq 1 18); do
        skip "N$i" "No GK1 API key"
    done
else
    WORKDIR=$(setup_workdir)
    STDERR_FILE="$TEST_TMP/stderr.log"

    cp "$SCRIPT_DIR/src/org-sim.naab" "$WORKDIR/"
    echo -e "${CYAN}Running 20-agent org simulation (~8-15 minutes with live API calls)...${NC}"
    echo -e "${CYAN}Config: 20 agents, lease=8, check_interval=2, max_unique_agents=21, pipeline_depth=8${NC}"
    echo ""

    OUTPUT=$(cd "$WORKDIR" && timeout 3600 "$NAAB" --governance-dashboard "org-sim.naab" 2>"$STDERR_FILE") && EXIT_CODE=0 || EXIT_CODE=$?

    echo ""
    echo -e "${CYAN}--- Output (last 80 lines) ---${NC}"
    echo "$OUTPUT" | tail -80
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

    MAX_AGENTS_ENFORCED=$(echo "$OUTPUT" | grep -oP 'SUMMARY_MAX_AGENTS_ENFORCED: \K\w+' | head -1)
    PIPELINE_SEP=$(echo "$OUTPUT" | grep -oP 'SUMMARY_PIPELINE_SEP: \K\w+' | head -1)

    FAN_OUT_5=$(echo "$OUTPUT" | grep -oP 'SUMMARY_FAN_OUT_5: \K[0-9]+' | head -1)
    FAN_OUT_5=${FAN_OUT_5:-0}

    MEGA_FAN_OUT=$(echo "$OUTPUT" | grep -oP 'SUMMARY_MEGA_FAN_OUT: \K\w+' | head -1)
    MEGA_COUNT=$(echo "$OUTPUT" | grep -oP 'SUMMARY_MEGA_FAN_OUT: \w+\|count=\K[0-9]+' | head -1)
    MEGA_COUNT=${MEGA_COUNT:-0}

    PIPELINE_6=$(echo "$OUTPUT" | grep -oP 'SUMMARY_PIPELINE_6: \K[0-9]+' | head -1)
    PIPELINE_6=${PIPELINE_6:-0}

    BACKEND_PC=$(echo "$OUTPUT" | grep -oP 'SUMMARY_BACKEND_PC: \K[0-9]+' | head -1)
    BACKEND_PC=${BACKEND_PC:-0}

    BACKEND_PRE=$(echo "$OUTPUT" | grep -oP 'SUMMARY_BACKEND_PRE_DRIFT: \K[0-9.e+-]+' | head -1)
    BACKEND_POST=$(echo "$OUTPUT" | grep -oP 'SUMMARY_BACKEND_POST_DRIFT: \K[0-9.e+-]+' | head -1)
    BACKEND_MIN=$(echo "$OUTPUT" | grep -oP 'SUMMARY_BACKEND_MIN_COHERENCE: \K[0-9.e+-]+' | head -1)

    CONSENSUS_OK=$(echo "$OUTPUT" | grep -oP 'SUMMARY_CONSENSUS: \K\w+' | head -1)
    CONSENSUS_VERDICT=$(echo "$OUTPUT" | grep -oP 'SUMMARY_CONSENSUS: \w+\|verdict=\K\w+' | head -1)

    AGENTS_WITH_DRIFT=$(echo "$OUTPUT" | grep -oP 'SUMMARY_AGENTS_WITH_DRIFT: \K[0-9]+' | head -1)
    AGENTS_WITH_DRIFT=${AGENTS_WITH_DRIFT:-0}

    TOTAL_CHALLENGES=$(echo "$OUTPUT" | grep -oP 'SUMMARY_TOTAL_CHALLENGES: \K[0-9]+' | head -1)
    TOTAL_CHALLENGES=${TOTAL_CHALLENGES:-0}

    CDD_ISOLATION=$(echo "$OUTPUT" | grep -oP 'SUMMARY_CDD_ISOLATION: \K[0-9]+' | head -1)
    CDD_ISOLATION=${CDD_ISOLATION:-0}
    CDD_ISOLATION_TOTAL=$(echo "$OUTPUT" | grep -oP 'SUMMARY_CDD_ISOLATION: [0-9]+/\K[0-9]+' | head -1)
    CDD_ISOLATION_TOTAL=${CDD_ISOLATION_TOTAL:-0}

    echo -e "${CYAN}Extracted: agents=$AGENTS_CREATED, sends=$TOTAL_SENDS, errors=$SEND_ERRORS${NC}"
    echo -e "${CYAN}          fan_out_5=$FAN_OUT_5/2, mega=$MEGA_FAN_OUT(${MEGA_COUNT}), pipeline_6=$PIPELINE_6/2${NC}"
    echo -e "${CYAN}          backend: pre=$BACKEND_PRE post=$BACKEND_POST min=$BACKEND_MIN pc=$BACKEND_PC${NC}"
    echo -e "${CYAN}          consensus=$CONSENSUS_VERDICT, drift_agents=$AGENTS_WITH_DRIFT, challenges=$TOTAL_CHALLENGES${NC}"
    echo ""

    # ============================================================
    # MA: MULTI-AGENT (MA01-MA08)
    # ============================================================
    echo -e "${CYAN}Multi-Agent Scaling${NC}"

    # MA01: All 20 agents created
    if [ "$AGENTS_CREATED" -eq 20 ]; then
        pass "MA01" "All 20 agents created"
    elif [ "$AGENTS_CREATED" -ge 15 ]; then
        pass "MA01" "$AGENTS_CREATED/20 agents created (acceptable)"
    else
        fail "MA01" "Agent creation failed" "only $AGENTS_CREATED/20 created"
    fi

    # MA02: max_unique_agents enforced
    if [ "${MAX_AGENTS_ENFORCED:-}" = "true" ]; then
        pass "MA02" "max_unique_agents enforcement works"
    else
        fail "MA02" "max_unique_agents not enforced"
    fi

    # MA03: Fan-out to 5 quality agents
    if [ "$FAN_OUT_5" -gt 0 ]; then
        pass "MA03" "Fan-out to 5 agents works ($FAN_OUT_5/2 rounds)"
    else
        fail "MA03" "Fan-out to 5 agents failed"
    fi

    # MA04: Mega fan-out to all 20
    if [ "${MEGA_FAN_OUT:-}" = "true" ]; then
        pass "MA04" "Mega fan-out to all $MEGA_COUNT agents works"
    elif [ "$MEGA_COUNT" -ge 15 ]; then
        pass "MA04" "Mega fan-out returned $MEGA_COUNT responses (acceptable)"
    else
        fail "MA04" "Mega fan-out failed" "count=$MEGA_COUNT"
    fi

    # MA05: 6-stage pipeline
    if [ "$PIPELINE_6" -gt 0 ]; then
        pass "MA05" "6-stage pipeline works ($PIPELINE_6/2 chains)"
    else
        fail "MA05" "6-stage pipeline failed"
    fi

    # MA06: Pipeline separation enforced
    if [ "${PIPELINE_SEP:-}" = "true" ]; then
        pass "MA06" "Pipeline separation enforced"
    else
        fail "MA06" "Pipeline separation not enforced"
    fi

    # MA07: Per-agent CDD isolation
    if [ "$CDD_ISOLATION" -gt 0 ] && [ "$CDD_ISOLATION_TOTAL" -gt 0 ]; then
        pass "MA07" "CDD isolation: $CDD_ISOLATION/$CDD_ISOLATION_TOTAL agents above drifted backend"
    else
        fail "MA07" "CDD isolation not detected" "isolation=$CDD_ISOLATION/$CDD_ISOLATION_TOTAL"
    fi

    # MA08: Consensus voting
    if [ "${CONSENSUS_OK:-}" = "true" ]; then
        pass "MA08" "Consensus voting works (verdict=$CONSENSUS_VERDICT)"
    else
        fail "MA08" "Consensus voting failed"
    fi

    # ============================================================
    # C: CORRECTNESS (C01-C04)
    # ============================================================
    echo -e "${CYAN}Correctness${NC}"

    if [ "$TOTAL_SENDS" -ge 80 ]; then
        pass "C01" "All sends completed ($TOTAL_SENDS total)"
    else
        fail "C01" "Insufficient sends" "got $TOTAL_SENDS, expected >= 80"
    fi

    if [ "$SEND_ERRORS" -le 10 ]; then
        pass "C02" "Low error rate ($SEND_ERRORS errors out of $TOTAL_SENDS sends)"
    else
        fail "C02" "Too many errors" "$SEND_ERRORS/$TOTAL_SENDS"
    fi

    if [ -n "${BACKEND_MIN:-}" ]; then
        BELOW_ONE=$(echo "$BACKEND_MIN < 1.0" | bc -l 2>/dev/null || echo "0")
        if [ "$BELOW_ONE" = "1" ]; then
            pass "C03" "Backend CDD varied (min=$BACKEND_MIN)"
        else
            fail "C03" "Backend coherence constant" "min=$BACKEND_MIN"
        fi
    else
        fail "C03" "No backend coherence data"
    fi

    if [ "$BACKEND_PC" -gt 0 ]; then
        pass "C04" "S20 fired for backend during drift (pc=$BACKEND_PC)"
    else
        fail "C04" "S20 did not fire" "backend_pc=$BACKEND_PC"
    fi

    # ============================================================
    # T: TELEMETRY (T01-T04)
    # ============================================================
    echo -e "${CYAN}Telemetry Integrity${NC}"

    HASH_CHAIN=$(echo "$OUTPUT" | grep -oP 'TELEM_HASH_CHAIN: \K\w+' | head -1)
    [ "${HASH_CHAIN:-}" = "true" ] && pass "T01" "Hash chain intact" || fail "T01" "Hash chain broken"

    TELEM_DIV=$(echo "$OUTPUT" | grep -oP 'TELEM_EVENT_DIVERSITY: \K[0-9]+' | head -1)
    [ "${TELEM_DIV:-0}" -ge 4 ] && pass "T02" "Event diversity ($TELEM_DIV types)" || fail "T02" "Low diversity ($TELEM_DIV)"

    RUN_ID=$(echo "$OUTPUT" | grep -oP 'TELEM_RUN_ID_CONSISTENT: \K\w+' | head -1)
    [ "${RUN_ID:-}" = "true" ] && pass "T03" "Run ID consistent" || fail "T03" "Run ID inconsistent"

    TS_OK=$(echo "$OUTPUT" | grep -oP 'TELEM_TIMESTAMPS_ORDERED: \K\w+' | head -1)
    [ "${TS_OK:-}" = "true" ] && pass "T04" "Timestamps ordered" || fail "T04" "Timestamps disordered"

    # ============================================================
    # H: HEALTH (H01-H02)
    # ============================================================
    echo -e "${CYAN}Health${NC}"

    TELEM_CHALLENGES=$(echo "$OUTPUT" | grep -oP 'TELEM_CHALLENGE_EVENTS: \K[0-9]+' | head -1)
    TELEM_CHALLENGES=${TELEM_CHALLENGES:-0}
    if [ "$TOTAL_CHALLENGES" -gt 0 ] || [ "$TELEM_CHALLENGES" -gt 0 ]; then
        pass "H01" "Challenges fired (summary=$TOTAL_CHALLENGES, telemetry=$TELEM_CHALLENGES)"
    else
        fail "H01" "No challenges fired"
    fi

    VERDICT=$(echo "$OUTPUT" | grep -oP 'HEALTH_VERDICT: \K\w+' | head -1)
    if [ "${VERDICT:-}" = "healthy" ] || [ "${VERDICT:-}" = "degraded" ]; then
        pass "H02" "Governance health: $VERDICT"
    else
        fail "H02" "Governance health: ${VERDICT:-unknown}"
    fi

    # ============================================================
    # AGENT STATES
    # ============================================================
    echo ""
    echo -e "${CYAN}Per-Agent Final State:${NC}"
    echo "$OUTPUT" | grep '^AGENT_STATE|' | while IFS='|' read -r _ name rest; do
        echo -e "  ${CYAN}$name $rest${NC}"
    done

    echo ""
    echo -e "${CYAN}Stderr Cross-Validation:${NC}"
    if [ -f "$STDERR_FILE" ]; then
        STDERR_SIZE=$(wc -c < "$STDERR_FILE")
        echo -e "  Stderr size: ${STDERR_SIZE} bytes"
        echo ""
        echo -e "${CYAN}--- Stderr (first 15 lines) ---${NC}"
        head -15 "$STDERR_FILE" 2>/dev/null
        echo -e "${CYAN}--- End Stderr ---${NC}"
    fi

    echo ""
    echo -e "${CYAN}Governance Health: ${VERDICT:-unknown}${NC}"
    echo -e "${CYAN}Consensus Verdict: ${CONSENSUS_VERDICT:-unknown}${NC}"
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
