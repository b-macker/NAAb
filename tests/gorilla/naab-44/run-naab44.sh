#!/usr/bin/env bash
# ============================================================
# naab-44: Long-Running Builder Agent — 200+ Turn Realism Test
#
# Runs a real 210-turn app-building conversation to test whether
# CDD + step-up recovery can sustain a long agent conversation.
# Assertions test GOVERNANCE CORRECTNESS, not LLM quality.
#
# Key insight: every agent.send() turn produces fingerprint "AS",
# so CDD circular detection fires on every check. The config is
# tuned (circular weight 0.04, adaptive baseline, step-up recovery)
# to sustain this without killing the agent. This test verifies
# that tuning actually works.
#
# Requires: GK1 env var with a Gemini API key
# Expected runtime: 60-120 minutes (210 turns x ~30s per turn for Gemma 4 31B free tier)
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/naab44-$$"
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
echo -e "${CYAN}|  naab-44: Long-Running Builder Agent — 200+ Turn Realism    |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

# ============================================================
# Check for API key
# ============================================================
if [ -z "${GK1:-}" ]; then
    echo -e "${YELLOW}No GK1 API key -- skipping all live long-build tests${NC}"
    for i in $(seq 1 20); do
        skip "N$i" "No GK1 API key"
    done
else
    WORKDIR=$(setup_workdir)
    STDERR_FILE="$TEST_TMP/stderr.log"

    # Copy long-build.naab and run
    cp "$SCRIPT_DIR/src/long-build.naab" "$WORKDIR/"
    echo -e "${CYAN}Running long-build (this takes 5-15 minutes with live API calls)...${NC}"
    echo -e "${CYAN}Config: circular_weight=0.06, check_interval=3, adaptive_baseline=3/1.5, lease=20, recovery=0.2${NC}"
    echo ""

    OUTPUT=$(cd "$WORKDIR" && timeout 14400 "$NAAB" --governance-dashboard "long-build.naab" 2>"$STDERR_FILE") && EXIT_CODE=0 || EXIT_CODE=$?

    echo ""
    echo -e "${CYAN}--- Output (last 60 lines) ---${NC}"
    echo "$OUTPUT" | tail -60
    echo -e "${CYAN}--- End Output ---${NC}"
    echo ""

    # ============================================================
    # Extract key values from output for reuse
    # ============================================================
    TURNS_COMPLETED=$(echo "$OUTPUT" | grep -oP 'SUMMARY_TURNS_COMPLETED: \K[0-9]+' | head -1)
    TURNS_COMPLETED=${TURNS_COMPLETED:-0}

    MIN_COHERENCE=$(echo "$OUTPUT" | grep -oP 'SUMMARY_MIN_COHERENCE: \K[0-9.e+-]+' | head -1)
    MIN_COHERENCE=${MIN_COHERENCE:-"unknown"}

    MAX_COHERENCE=$(echo "$OUTPUT" | grep -oP 'SUMMARY_MAX_COHERENCE: \K[0-9.e+-]+' | head -1)
    MAX_COHERENCE=${MAX_COHERENCE:-"unknown"}

    LAST_COHERENCE=$(echo "$OUTPUT" | grep -oP 'SUMMARY_LAST_COHERENCE: \K[0-9.e+-]+' | head -1)
    LAST_COHERENCE=${LAST_COHERENCE:-"unknown"}

    CHALLENGES_PASSED=$(echo "$OUTPUT" | grep -oP 'SUMMARY_CHALLENGES_PASSED: \K[0-9]+' | head -1)
    CHALLENGES_PASSED=${CHALLENGES_PASSED:-0}

    CHALLENGES_FAILED=$(echo "$OUTPUT" | grep -oP 'SUMMARY_CHALLENGES_FAILED: \K[0-9]+' | head -1)
    CHALLENGES_FAILED=${CHALLENGES_FAILED:-0}

    SEND_ERRORS=$(echo "$OUTPUT" | grep -oP 'SUMMARY_SEND_ERRORS: \K[0-9]+' | head -1)
    SEND_ERRORS=${SEND_ERRORS:-0}

    PHASE_REACHED=$(echo "$OUTPUT" | grep -oP 'SUMMARY_PHASE_REACHED: \K\w+' | head -1)
    PHASE_REACHED=${PHASE_REACHED:-"NONE"}

    RECOVERY_SEEN=$(echo "$OUTPUT" | grep -oP 'SUMMARY_RECOVERY_SEEN: \K\w+' | head -1)
    RECOVERY_SEEN=${RECOVERY_SEEN:-"false"}

    echo -e "${CYAN}Extracted: turns=$TURNS_COMPLETED, coherence=[$MIN_COHERENCE..$MAX_COHERENCE], last=$LAST_COHERENCE${NC}"
    echo -e "${CYAN}          challenges: passed=$CHALLENGES_PASSED failed=$CHALLENGES_FAILED errors=$SEND_ERRORS${NC}"
    echo -e "${CYAN}          phase=$PHASE_REACHED recovery=$RECOVERY_SEEN${NC}"
    echo ""

    # ============================================================
    # S: SYSTEM INTEGRITY (S01-S04)
    # ============================================================
    echo -e "${CYAN}System Integrity${NC}"

    # S01: Program completed (exit 0 or governance error, not crash/segfault)
    if [ "$EXIT_CODE" -eq 0 ] || [ "$EXIT_CODE" -eq 2 ] || [ "$EXIT_CODE" -eq 3 ]; then
        pass "S01" "Program completed (exit $EXIT_CODE)"
    elif [ "$EXIT_CODE" -eq 124 ]; then
        fail "S01" "Program timed out (4-hour limit)"
    else
        fail "S01" "Program crashed" "exit code $EXIT_CODE"
    fi

    # S02: Monitoring data present (at least 10 TURN| lines)
    TURN_LINES=$(echo "$OUTPUT" | grep -c '^TURN|' || true)
    if [ "$TURN_LINES" -ge 10 ]; then
        pass "S02" "Monitoring data present ($TURN_LINES TURN lines)"
    else
        fail "S02" "Insufficient monitoring data" "only $TURN_LINES TURN lines (need >= 10)"
    fi

    # S03: Summary output present
    SUMMARY_LINES=$(echo "$OUTPUT" | grep -c '^SUMMARY_' || true)
    if [ "$SUMMARY_LINES" -ge 5 ]; then
        pass "S03" "Summary output present ($SUMMARY_LINES SUMMARY lines)"
    else
        fail "S03" "Missing summary output" "only $SUMMARY_LINES SUMMARY lines"
    fi

    # S04: Completed within timeout (not 124)
    if [ "$EXIT_CODE" -ne 124 ]; then
        pass "S04" "Completed within 4-hour timeout"
    else
        fail "S04" "Exceeded 4-hour timeout"
    fi

    # ============================================================
    # C: CDD CORRECTNESS (C01-C05)
    # ============================================================
    echo -e "${CYAN}CDD Correctness${NC}"

    # C01: Coherence was NOT constant — must see at least 2 different values
    # Proves CDD applied penalties, not just reported 1.0 forever.
    UNIQUE_COHERENCES=$(echo "$OUTPUT" | grep '^TURN|' | grep -oP '\|c=\K[0-9.e+-]+' | sort -u | wc -l)
    if [ "$UNIQUE_COHERENCES" -ge 2 ]; then
        pass "C01" "Coherence varied ($UNIQUE_COHERENCES distinct values)"
    else
        fail "C01" "Coherence was constant" "only $UNIQUE_COHERENCES distinct value(s) -- CDD not applying penalties?"
    fi

    # C02: If survived past turn 50 (beyond adaptive baseline window of 10),
    #       coherence MUST have dropped below 1.0 at some point
    if [ "$TURNS_COMPLETED" -gt 50 ]; then
        # min_coherence should be less than 1.0 if penalties fired post-baseline
        if [ "$MIN_COHERENCE" != "unknown" ]; then
            BELOW_ONE=$(echo "$MIN_COHERENCE < 1.0" | bc -l 2>/dev/null || echo "0")
            if [ "$BELOW_ONE" = "1" ]; then
                pass "C02" "Coherence dropped below 1.0 (min=$MIN_COHERENCE)"
            else
                fail "C02" "Coherence never dropped below 1.0" "min=$MIN_COHERENCE -- penalties not firing post-baseline?"
            fi
        else
            fail "C02" "Could not read min coherence"
        fi
    else
        skip "C02" "Agent didn't survive past turn 50 (completed $TURNS_COMPLETED)"
    fi

    # C03: Coherence never went negative (runtime clamped at 0.0)
    if [ "$MIN_COHERENCE" != "unknown" ]; then
        NEGATIVE=$(echo "$MIN_COHERENCE < 0" | bc -l 2>/dev/null || echo "0")
        if [ "$NEGATIVE" = "0" ]; then
            pass "C03" "Coherence never negative (min=$MIN_COHERENCE)"
        else
            fail "C03" "Coherence went negative" "min=$MIN_COHERENCE -- clamp broken?"
        fi
    else
        fail "C03" "Could not read min coherence"
    fi

    # C04: CDD_TURN events present in telemetry (proves CDD actually ran)
    TELEM_CDD=$(echo "$OUTPUT" | grep -oP 'TELEM_CDD_EVENTS: \K[0-9]+' | head -1)
    TELEM_CDD=${TELEM_CDD:-0}
    if [ "$TELEM_CDD" -gt 0 ]; then
        pass "C04" "CDD_TURN events in telemetry ($TELEM_CDD events)"
    else
        fail "C04" "No CDD_TURN events in telemetry"
    fi

    # C05: At least some CDD events had signals fire (proves penalties work).
    # With adaptive baselining, most CDD checks won't fire penalties — signals
    # must exceed mean + k*stddev to penalize. So we just check signals > 0.
    TELEM_CDD_SIGNALS=$(echo "$OUTPUT" | grep -oP 'TELEM_CDD_SIGNAL_EVENTS: \K[0-9]+' | head -1)
    TELEM_CDD_SIGNALS=${TELEM_CDD_SIGNALS:-0}
    if [ "$TURNS_COMPLETED" -gt 0 ]; then
        if [ "$TELEM_CDD_SIGNALS" -gt 0 ]; then
            pass "C05" "CDD signals fired ($TELEM_CDD_SIGNALS events with penalties)"
        else
            fail "C05" "No CDD signals fired in $TURNS_COMPLETED turns" "adaptive baseline may be too forgiving"
        fi
    else
        skip "C05" "No turns to check"
    fi

    # ============================================================
    # CH: CHALLENGE CORRECTNESS (CH01-CH03)
    # ============================================================
    echo -e "${CYAN}Challenge Correctness${NC}"

    # CH01: If survived 20+ turns, at least one challenge event in telemetry
    # Standing lease is 20 turns — after that, step-up challenge MUST fire
    TELEM_CPASS=$(echo "$OUTPUT" | grep -oP 'TELEM_CHALLENGE_PASS_EVENTS: \K[0-9]+' | head -1)
    TELEM_CPASS=${TELEM_CPASS:-0}
    TELEM_CFAIL=$(echo "$OUTPUT" | grep -oP 'TELEM_CHALLENGE_FAIL_EVENTS: \K[0-9]+' | head -1)
    TELEM_CFAIL=${TELEM_CFAIL:-0}
    TOTAL_CHALLENGE_EVENTS=$((TELEM_CPASS + TELEM_CFAIL))

    if [ "$TURNS_COMPLETED" -ge 20 ]; then
        if [ "$TOTAL_CHALLENGE_EVENTS" -gt 0 ]; then
            pass "CH01" "Challenge events in telemetry ($TELEM_CPASS pass, $TELEM_CFAIL fail)"
        else
            fail "CH01" "No challenge events despite $TURNS_COMPLETED turns" "lease=20 should have expired"
        fi
    else
        skip "CH01" "Agent didn't survive 20 turns (completed $TURNS_COMPLETED)"
    fi

    # CH02: Challenge pass/fail counts from summary match telemetry event counts
    if [ "$TOTAL_CHALLENGE_EVENTS" -gt 0 ]; then
        MATCH=true
        if [ "$CHALLENGES_PASSED" != "$TELEM_CPASS" ]; then MATCH=false; fi
        if [ "$CHALLENGES_FAILED" != "$TELEM_CFAIL" ]; then MATCH=false; fi
        if [ "$MATCH" = true ]; then
            pass "CH02" "Challenge counters consistent (summary vs telemetry)"
        else
            fail "CH02" "Challenge counter mismatch" "summary: pass=$CHALLENGES_PASSED fail=$CHALLENGES_FAILED, telemetry: pass=$TELEM_CPASS fail=$TELEM_CFAIL"
        fi
    else
        skip "CH02" "No challenge events to cross-validate"
    fi

    # CH03: If any challenge passed AND recovery was observed -> coherence increased
    # This proves recoverCoherence() actually works (behavioral_sequence.cpp:1077-1088)
    if [ "$CHALLENGES_PASSED" -gt 0 ]; then
        if [ "$RECOVERY_SEEN" = "true" ]; then
            pass "CH03" "Challenge recovery observed (coherence increased after pass)"
        else
            # Recovery might not be observed if coherence was already near 1.0
            # or if we couldn't capture the before/after coherence.
            # This is a genuine test — failure reveals a broken recovery path.
            RECOVERY_LINES=$(echo "$OUTPUT" | grep -c '^RECOVERY|' || true)
            if [ "$RECOVERY_LINES" -gt 0 ]; then
                pass "CH03" "Recovery events detected ($RECOVERY_LINES)"
            else
                fail "CH03" "No recovery observed despite $CHALLENGES_PASSED challenge passes" "recoverCoherence() may be broken, or coherence not captured pre/post"
            fi
        fi
    else
        skip "CH03" "No challenge passes to verify recovery"
    fi

    # ============================================================
    # T: TELEMETRY INTEGRITY (T01-T04)
    # ============================================================
    echo -e "${CYAN}Telemetry Integrity${NC}"

    # T01: Hash chain intact
    HASH_CHAIN=$(echo "$OUTPUT" | grep -oP 'TELEM_HASH_CHAIN: \K\w+' | head -1)
    if [ "${HASH_CHAIN:-}" = "true" ]; then
        pass "T01" "Telemetry hash chain intact"
    else
        fail "T01" "Hash chain broken" "TELEM_HASH_CHAIN=$HASH_CHAIN"
    fi

    # T02: 4+ distinct event types
    TELEM_DIVERSITY=$(echo "$OUTPUT" | grep -oP 'TELEM_EVENT_DIVERSITY: \K[0-9]+' | head -1)
    TELEM_DIVERSITY=${TELEM_DIVERSITY:-0}
    if [ "$TELEM_DIVERSITY" -ge 4 ]; then
        pass "T02" "Event type diversity ($TELEM_DIVERSITY distinct types)"
    else
        fail "T02" "Insufficient event diversity" "only $TELEM_DIVERSITY types (need >= 4)"
    fi

    # T03: Run ID consistent across all events
    RUN_ID_OK=$(echo "$OUTPUT" | grep -oP 'TELEM_RUN_ID_CONSISTENT: \K\w+' | head -1)
    if [ "${RUN_ID_OK:-}" = "true" ]; then
        pass "T03" "Run ID consistent across all events"
    else
        fail "T03" "Run ID inconsistency"
    fi

    # T04: Timestamps non-decreasing
    TS_ORDERED=$(echo "$OUTPUT" | grep -oP 'TELEM_TIMESTAMPS_ORDERED: \K\w+' | head -1)
    if [ "${TS_ORDERED:-}" = "true" ]; then
        pass "T04" "Timestamps non-decreasing"
    else
        fail "T04" "Timestamp ordering violated"
    fi

    # ============================================================
    # D: DRIFT ZONE ANALYSIS (D01-D02) — Reports, not hard assertions
    # ============================================================
    echo -e "${CYAN}Drift Zone Analysis${NC}"

    # D01: Compare coherence slope in drift zone (131-160) vs clean zone (31-80)
    # Since CDD can't detect content drift (fingerprint always "AS"),
    # slopes SHOULD be similar. Different slopes would be a finding.
    DRIFT_DELTA=$(echo "$OUTPUT" | grep 'PHASE_DELTA|DRIFT' | grep -oP 'delta=\K[0-9.e+-]+' | head -1)
    CORE_DELTA=$(echo "$OUTPUT" | grep 'PHASE_DELTA|CORE_BUILD' | grep -oP 'delta=\K[0-9.e+-]+' | head -1)
    if [ -n "${DRIFT_DELTA:-}" ] && [ -n "${CORE_DELTA:-}" ]; then
        echo -e "  ${CYAN}REPORT${NC} [D01] Coherence slopes: CORE_BUILD delta=$CORE_DELTA, DRIFT delta=$DRIFT_DELTA"
        echo -e "         ${CYAN}(Similar slopes = CDD is content-blind as expected)${NC}"
        pass "D01" "Drift zone coherence data available"
    elif [ -n "${CORE_DELTA:-}" ]; then
        echo -e "  ${CYAN}REPORT${NC} [D01] CORE_BUILD delta=$CORE_DELTA (agent didn't reach drift zone)"
        pass "D01" "Partial coherence data available (phase=$PHASE_REACHED)"
    else
        # If agent died before core build phase, still pass — D01 is a report
        echo -e "  ${CYAN}REPORT${NC} [D01] Insufficient data for slope comparison (turns=$TURNS_COMPLETED)"
        pass "D01" "Report generated (insufficient data)"
    fi

    # D02: Total turns completed — report, always passes
    echo -e "  ${CYAN}REPORT${NC} [D02] Total turns completed: $TURNS_COMPLETED / 210"
    echo -e "         ${CYAN}Phase reached: $PHASE_REACHED${NC}"
    echo -e "         ${CYAN}Send errors: $SEND_ERRORS, Challenges: $CHALLENGES_PASSED pass / $CHALLENGES_FAILED fail${NC}"
    pass "D02" "Completion report generated ($TURNS_COMPLETED turns, phase=$PHASE_REACHED)"

    # ============================================================
    # PHASE COHERENCE TRAJECTORY
    # ============================================================
    echo ""
    echo -e "${CYAN}Phase Coherence Trajectory:${NC}"
    echo "$OUTPUT" | grep '^PHASE_DELTA|' | while IFS= read -r line; do
        echo -e "  ${CYAN}$line${NC}"
    done

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

        if grep -qi "level\|elevated\|circuit\|step.up\|challenge" "$STDERR_FILE"; then
            echo -e "  ${GREEN}OK${NC} Level change / step-up activity in stderr"
        else
            echo -e "  ${YELLOW}!${NC} No level change in stderr"
        fi

        if grep -qi "lease\|renew" "$STDERR_FILE"; then
            echo -e "  ${GREEN}OK${NC} Lease activity in stderr"
        else
            echo -e "  ${YELLOW}!${NC} No lease activity in stderr"
        fi

        echo ""
        echo -e "${CYAN}--- Stderr (first 30 lines) ---${NC}"
        head -30 "$STDERR_FILE" 2>/dev/null
        echo -e "${CYAN}--- End Stderr ---${NC}"
    else
        echo -e "  ${YELLOW}!${NC} No stderr file captured"
    fi

    # ============================================================
    # HEALTH VERDICT
    # ============================================================
    echo ""
    VERDICT=$(echo "$OUTPUT" | grep -oP 'HEALTH_VERDICT: \K\w+' | head -1)
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

# Save full output for post-mortem
if [ -n "${OUTPUT:-}" ]; then
    echo "$OUTPUT" > "$RESULTS_DIR/results_${TIMESTAMP}.txt"
fi
if [ -f "${STDERR_FILE:-/dev/null}" ]; then
    cp "$STDERR_FILE" "$RESULTS_DIR/stderr_${TIMESTAMP}.txt" 2>/dev/null || true
fi

# Copy telemetry + transcript for manual audit
if [ -f "${WORKDIR:-/dev/null}/telemetry.jsonl" ]; then
    cp "$WORKDIR/telemetry.jsonl" "$RESULTS_DIR/telemetry_${TIMESTAMP}.jsonl" 2>/dev/null || true
fi
if [ -f "${WORKDIR:-/dev/null}/transcript.jsonl" ]; then
    cp "$WORKDIR/transcript.jsonl" "$RESULTS_DIR/transcript_${TIMESTAMP}.jsonl" 2>/dev/null || true
fi

exit "$FAIL_COUNT"
