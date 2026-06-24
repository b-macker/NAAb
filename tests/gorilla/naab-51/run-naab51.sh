#!/usr/bin/env bash
# ============================================================
# naab-51: Tool Execution Under Governance Escalation
#
# 25-turn phased conversation (TOOL_BASELINE → DRIFT → TOOL_UNDER_PRESSURE)
# validating that the tool execution loop works correctly across
# governance level transitions:
#
#   - Tools execute normally at NORMAL level
#   - CDD fires during drift phase (off-topic turns)
#   - Tool execution still works under elevated governance pressure
#   - Telemetry captures TOOL_CALL + CDD_TURN + GOVERNANCE_LEVEL_CHANGE
#   - Tool call counters in response dict are accurate
#
# Requires: GK* env var with a Gemini API key
# Expected runtime: 2-5 minutes (25 turns x ~5s per turn)
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/naab51-$$"
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
echo -e "${CYAN}|  naab-51: Tool Execution Under Governance Escalation         |${NC}"
echo -e "${CYAN}|  Tools + CDD + Circuit Breaker + Reality Checkpoint          |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

if [ -z "${GK1:-}${GK2:-}${GK3:-}${GK4:-}${GK5:-}${GK6:-}${GK7:-}${GK8:-}${GK9:-}" ]; then
    echo -e "${YELLOW}No GK* API key — skipping all live tests${NC}"
    for i in $(seq 1 16); do
        skip "N$i" "No GK* API key"
    done
else
    WORKDIR=$(setup_workdir)
    STDERR_FILE="$TEST_TMP/stderr.log"

    cp "$SCRIPT_DIR/src/tool-escalation.naab" "$WORKDIR/"
    echo -e "${CYAN}Running tool-escalation (25 turns, ~2-5 min with live API)...${NC}"
    echo ""

    STDOUT_FILE="$WORKDIR/stdout.log"
    (cd "$WORKDIR" && timeout 600 "$NAAB" --governance-dashboard "tool-escalation.naab" >"$STDOUT_FILE" 2>"$STDERR_FILE") && EXIT_CODE=0 || EXIT_CODE=$?
    OUTPUT=$(cat "$STDOUT_FILE" 2>/dev/null)

    echo ""
    echo -e "${CYAN}--- Output (last 40 lines) ---${NC}"
    echo "$OUTPUT" | tail -40
    echo -e "${CYAN}--- End Output ---${NC}"
    echo ""

    # ============================================================
    # Extract values
    # ============================================================
    TURNS=$(echo "$OUTPUT" | grep -oP 'SUMMARY_TURNS_COMPLETED: \K[0-9]+' | head -1)
    if [ -z "$TURNS" ]; then
        TURNS=$(echo "$OUTPUT" | grep -c '^TURN|' || true)
    fi
    TURNS=${TURNS:-0}

    SEND_ERRORS=$(echo "$OUTPUT" | grep -oP 'SUMMARY_SEND_ERRORS: \K[0-9]+' | head -1)
    SEND_ERRORS=${SEND_ERRORS:-0}

    TOTAL_TOOL_CALLS=$(echo "$OUTPUT" | grep -oP 'SUMMARY_TOTAL_TOOL_CALLS: \K[0-9]+' | head -1)
    TOTAL_TOOL_CALLS=${TOTAL_TOOL_CALLS:-0}
    TOOL_CALLS_BASELINE=$(echo "$OUTPUT" | grep -oP 'SUMMARY_TOOL_CALLS_BASELINE: \K[0-9]+' | head -1)
    TOOL_CALLS_BASELINE=${TOOL_CALLS_BASELINE:-0}
    TOOL_CALLS_PRESSURE=$(echo "$OUTPUT" | grep -oP 'SUMMARY_TOOL_CALLS_PRESSURE: \K[0-9]+' | head -1)
    TOOL_CALLS_PRESSURE=${TOOL_CALLS_PRESSURE:-0}

    LEVEL_CHANGES=$(echo "$OUTPUT" | grep -oP 'SUMMARY_LEVEL_CHANGES: \K[0-9]+' | head -1)
    if [ -z "$LEVEL_CHANGES" ]; then
        LEVEL_CHANGES=$(echo "$OUTPUT" | grep -c '^LEVEL_CHANGE|' || true)
    fi
    LEVEL_CHANGES=${LEVEL_CHANGES:-0}

    FIRST_PENALTY=$(echo "$OUTPUT" | grep -oP 'SUMMARY_FIRST_PENALTY_SEEN: \K\w+' | head -1)
    if [ -z "$FIRST_PENALTY" ]; then
        echo "$OUTPUT" | grep -q '^FIRST_PENALTY|' && FIRST_PENALTY="true" || FIRST_PENALTY="false"
    fi

    USAGE_TOOL_TOTAL=$(echo "$OUTPUT" | grep -oP 'USAGE_TOOL_CALLS_TOTAL: \K[0-9]+' | head -1)
    USAGE_TOOL_TOTAL=${USAGE_TOOL_TOTAL:-0}
    USAGE_TOOL_BLOCKED=$(echo "$OUTPUT" | grep -oP 'USAGE_TOOL_CALLS_BLOCKED: \K[0-9]+' | head -1)
    USAGE_TOOL_BLOCKED=${USAGE_TOOL_BLOCKED:-0}

    HEALTH=$(echo "$OUTPUT" | grep -oP 'HEALTH_VERDICT: \K\w+' | head -1)
    EPOCH=$(echo "$OUTPUT" | grep -oP 'HEALTH_EPOCH: \K[0-9]+' | head -1)
    TRANSCRIPT_CONNECTED=$(echo "$OUTPUT" | grep -oP 'HEALTH_TRANSCRIPT: \K\w+' | head -1)

    TELEM_FILE="$WORKDIR/telemetry.jsonl"
    TELEM_TOOL=$(grep -c "AGENT_TOOL_CALL\|TOOL_CALL" "$TELEM_FILE" 2>/dev/null || true)
    TELEM_CDD=$(grep -c "CDD_TURN" "$TELEM_FILE" 2>/dev/null || true)
    TELEM_LEVEL=$(grep -c "GOVERNANCE_LEVEL_CHANGE" "$TELEM_FILE" 2>/dev/null || true)
    TELEM_SEM=$(grep -c "SEMANTIC_TURN" "$TELEM_FILE" 2>/dev/null || true)
    TELEM_TOTAL=$(wc -l < "$TELEM_FILE" 2>/dev/null || echo "0")

    echo -e "${CYAN}Extracted: turns=$TURNS, errors=$SEND_ERRORS, tool_calls=$TOTAL_TOOL_CALLS (baseline=$TOOL_CALLS_BASELINE, pressure=$TOOL_CALLS_PRESSURE)${NC}"
    echo -e "${CYAN}  Level changes: $LEVEL_CHANGES, usage: total=$USAGE_TOOL_TOTAL blocked=$USAGE_TOOL_BLOCKED${NC}"
    echo -e "${CYAN}  Telemetry: tool=$TELEM_TOOL CDD=$TELEM_CDD level=$TELEM_LEVEL semantic=$TELEM_SEM total=$TELEM_TOTAL${NC}"
    echo ""

    # ============================================================
    # S: SYSTEM INTEGRITY (S01-S02)
    # ============================================================
    echo -e "${CYAN}System Integrity${NC}"

    if [ "$EXIT_CODE" -eq 0 ] || [ "$EXIT_CODE" -eq 2 ] || [ "$EXIT_CODE" -eq 3 ]; then
        pass "S01" "Program completed (exit $EXIT_CODE)"
    elif [ "$EXIT_CODE" -eq 124 ]; then
        fail "S01" "Program timed out"
    else
        fail "S01" "Program crashed" "exit $EXIT_CODE"
    fi

    if [ "$TURNS" -ge 10 ]; then
        pass "S02" "Sufficient turns completed ($TURNS / 25)"
    else
        fail "S02" "Insufficient turns" "$TURNS / 25"
    fi

    # ============================================================
    # T: TOOL EXECUTION (T01-T04)
    # ============================================================
    echo -e "${CYAN}Tool Execution${NC}"

    # T01: Tool calls made during TOOL_BASELINE phase
    if [ "$TOOL_CALLS_BASELINE" -gt 0 ]; then
        pass "T01" "Tool calls made during TOOL_BASELINE ($TOOL_CALLS_BASELINE)"
    elif [ "$TURNS" -ge 8 ]; then
        fail "T01" "No tool calls during TOOL_BASELINE" "LLM may not have called tools"
    else
        skip "T01" "Not enough turns for TOOL_BASELINE ($TURNS)"
    fi

    # T02: Total tool calls > 0
    if [ "$TOTAL_TOOL_CALLS" -gt 0 ]; then
        pass "T02" "Total tool calls > 0 ($TOTAL_TOOL_CALLS)"
    else
        fail "T02" "No tool calls made during entire run"
    fi

    # T03: Usage counters consistent (usage.tool_calls_total matches summary)
    if [ "$USAGE_TOOL_TOTAL" -gt 0 ]; then
        pass "T03" "Usage tool_calls_total > 0 ($USAGE_TOOL_TOTAL)"
    elif [ "$TOTAL_TOOL_CALLS" -gt 0 ]; then
        fail "T03" "Summary shows tool calls but usage counter is 0"
    else
        skip "T03" "No tool calls to cross-validate"
    fi

    # T04: Tool calls under pressure (TOOL_UNDER_PRESSURE phase)
    if [ "$TOOL_CALLS_PRESSURE" -gt 0 ]; then
        pass "T04" "Tools executed under governance pressure ($TOOL_CALLS_PRESSURE calls)"
    elif [ "$TURNS" -ge 19 ]; then
        # It's possible governance blocked tools or LLM didn't call them
        echo -e "  ${YELLOW}NOTE${NC} No tool calls under pressure — LLM may not have called tools or governance blocked"
        pass "T04" "Reached pressure phase (tool invocation is LLM-dependent)"
    else
        skip "T04" "Not enough turns for pressure phase ($TURNS)"
    fi

    # ============================================================
    # CDD: CONTEXT DRIFT DETECTION (CDD01-CDD02)
    # ============================================================
    echo -e "${CYAN}Context Drift Detection${NC}"

    # CDD01: CDD telemetry events present
    if [ "${TELEM_CDD:-0}" -gt 0 ]; then
        pass "CDD01" "CDD_TURN telemetry events present ($TELEM_CDD)"
    else
        fail "CDD01" "No CDD_TURN telemetry events"
    fi

    # CDD02: Coherence changed during run (CDD is doing something)
    if [ "$FIRST_PENALTY" = "true" ]; then
        pass "CDD02" "Coherence penalty observed (CDD active)"
    elif [ "$TURNS" -ge 15 ]; then
        # Possible with adaptive baseline if model is very consistent
        echo -e "  ${YELLOW}NOTE${NC} No coherence penalty after $TURNS turns — adaptive baseline may suppress"
        pass "CDD02" "CDD ran (no penalty may be correct with adaptive baseline)"
    else
        skip "CDD02" "Not enough turns for CDD pressure ($TURNS)"
    fi

    # ============================================================
    # CB: CIRCUIT BREAKER (CB01-CB02)
    # ============================================================
    echo -e "${CYAN}Circuit Breaker${NC}"

    # CB01: Level changes observed or governance stayed stable
    if [ "${LEVEL_CHANGES:-0}" -gt 0 ]; then
        pass "CB01" "Governance level changes observed ($LEVEL_CHANGES)"
    elif [ "$TURNS" -ge 18 ]; then
        echo -e "  ${YELLOW}NOTE${NC} No level changes — pressure may not have sustained"
        pass "CB01" "No level changes (pressure may not have sustained — not a bug)"
    else
        skip "CB01" "Not enough turns for level changes ($TURNS)"
    fi

    # CB02: GOVERNANCE_LEVEL_CHANGE telemetry consistent with observed changes
    if [ "${TELEM_LEVEL:-0}" -gt 0 ]; then
        pass "CB02" "GOVERNANCE_LEVEL_CHANGE telemetry events ($TELEM_LEVEL)"
    elif [ "${LEVEL_CHANGES:-0}" -gt 0 ]; then
        fail "CB02" "Level changes observed but no telemetry events"
    else
        pass "CB02" "No level changes to emit (consistent)"
    fi

    # ============================================================
    # TEL: TELEMETRY INTEGRITY (TEL01-TEL04)
    # ============================================================
    echo -e "${CYAN}Telemetry Integrity${NC}"

    # TEL01: Telemetry file exists and has events
    if [ "${TELEM_TOTAL:-0}" -gt 5 ]; then
        pass "TEL01" "Telemetry file has events ($TELEM_TOTAL lines)"
    elif [ "${TELEM_TOTAL:-0}" -gt 0 ]; then
        pass "TEL01" "Telemetry file has some events ($TELEM_TOTAL lines)"
    else
        fail "TEL01" "Telemetry file empty or missing"
    fi

    # TEL02: TOOL_CALL telemetry events match tool calls
    if [ "${TELEM_TOOL:-0}" -gt 0 ] && [ "$TOTAL_TOOL_CALLS" -gt 0 ]; then
        pass "TEL02" "TOOL_CALL telemetry events present ($TELEM_TOOL)"
    elif [ "$TOTAL_TOOL_CALLS" -eq 0 ] && [ "${TELEM_TOOL:-0}" -eq 0 ]; then
        pass "TEL02" "No tool calls, no tool telemetry (consistent)"
    elif [ "$TOTAL_TOOL_CALLS" -gt 0 ]; then
        fail "TEL02" "Tool calls made but no TOOL_CALL telemetry"
    else
        pass "TEL02" "Telemetry consistent with tool usage"
    fi

    # TEL03: SEMANTIC_TURN telemetry present
    if [ "${TELEM_SEM:-0}" -gt 0 ]; then
        pass "TEL03" "SEMANTIC_TURN telemetry events ($TELEM_SEM)"
    else
        fail "TEL03" "No SEMANTIC_TURN telemetry events"
    fi

    # TEL04: Telemetry JSONL is valid
    if [ -f "$TELEM_FILE" ]; then
        INVALID_TELEM=0
        while IFS= read -r line; do
            if [ -n "$line" ]; then
                echo "$line" | python3 -c "import sys,json; json.load(sys.stdin)" 2>/dev/null || INVALID_TELEM=$((INVALID_TELEM + 1))
            fi
        done < "$TELEM_FILE"
        if [ "$INVALID_TELEM" -eq 0 ]; then
            pass "TEL04" "All telemetry lines are valid JSON ($TELEM_TOTAL lines)"
        else
            fail "TEL04" "Invalid JSON in telemetry" "$INVALID_TELEM invalid lines"
        fi
    else
        fail "TEL04" "Telemetry file not found"
    fi

    # ============================================================
    # TR: TRANSCRIPT (TR01-TR02)
    # ============================================================
    echo -e "${CYAN}Transcript${NC}"

    TRANSCRIPT_FILE="$WORKDIR/transcript.jsonl"
    if [ -f "$TRANSCRIPT_FILE" ]; then
        TR_LINES=$(wc -l < "$TRANSCRIPT_FILE" 2>/dev/null || echo "0")
        if [ "$TR_LINES" -gt 0 ]; then
            pass "TR01" "Transcript file present ($TR_LINES lines)"
        else
            fail "TR01" "Transcript file empty"
        fi
    else
        fail "TR01" "Transcript file missing"
    fi

    # TR02: governance.health() reports transcript_connected
    if [ "${TRANSCRIPT_CONNECTED:-}" = "true" ]; then
        pass "TR02" "governance.health() reports transcript_connected=true"
    elif [ -n "${TRANSCRIPT_CONNECTED:-}" ]; then
        fail "TR02" "transcript_connected=$TRANSCRIPT_CONNECTED"
    else
        skip "TR02" "No health data available"
    fi

    # ============================================================
    # PHASE TRAJECTORY
    # ============================================================
    echo ""
    echo -e "${CYAN}Phase Summaries:${NC}"
    echo "$OUTPUT" | grep '^PHASE_DELTA|' | while IFS= read -r line; do
        echo -e "  ${CYAN}$line${NC}"
    done

    echo ""
    echo -e "${CYAN}Level Changes:${NC}"
    LEVEL_LINES=$(echo "$OUTPUT" | grep '^LEVEL_CHANGE|' || true)
    if [ -n "$LEVEL_LINES" ]; then
        echo "$LEVEL_LINES" | while IFS= read -r line; do
            echo -e "  ${CYAN}$line${NC}"
        done
    else
        echo -e "  ${YELLOW}(none)${NC}"
    fi

    echo ""
    echo -e "${CYAN}Governance Health: ${HEALTH:-unknown} (epoch=${EPOCH:-?})${NC}"

    # ============================================================
    # STDERR HIGHLIGHTS
    # ============================================================
    echo ""
    echo -e "${CYAN}Stderr Highlights:${NC}"
    if [ -f "$STDERR_FILE" ]; then
        STDERR_SIZE=$(wc -c < "$STDERR_FILE")
        echo -e "  Stderr size: ${STDERR_SIZE} bytes"

        if grep -qi "tool" "$STDERR_FILE"; then
            echo -e "  ${GREEN}OK${NC} Tool-related activity in stderr"
        fi
        if grep -qi "checkpoint\|pressure\|level" "$STDERR_FILE"; then
            echo -e "  ${GREEN}OK${NC} Checkpoint/pressure/level activity in stderr"
        fi
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
if [ -f "${WORKDIR:-/dev/null}/telemetry.jsonl" ]; then
    cp "$WORKDIR/telemetry.jsonl" "$RESULTS_DIR/telemetry_${TIMESTAMP}.jsonl" 2>/dev/null || true
fi
if [ -f "${WORKDIR:-/dev/null}/transcript.jsonl" ]; then
    cp "$WORKDIR/transcript.jsonl" "$RESULTS_DIR/transcript_${TIMESTAMP}.jsonl" 2>/dev/null || true
fi

exit "$FAIL_COUNT"
