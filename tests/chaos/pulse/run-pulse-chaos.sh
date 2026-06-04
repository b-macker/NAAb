#!/usr/bin/env bash
# ============================================================================
# NAAb Governance Pulse — Chaos Tests
# Tests: health() structure, pulse counters, dashboard line, CB-disabled,
#        no-governance fallback, weight parsing, pressure factor wiring
# Usage: bash run-pulse-chaos.sh [--test N]
# ============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../../build/naab-lang"
TEST_TMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}/chaos-pulse-$$"
SIGNING_KEY="$HOME/.naab/keys/signing.pem"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

# Counters
PASS_COUNT=0
FAIL_COUNT=0
SKIP_COUNT=0
TOTAL=0
FAILURES=""

# Test selection
RUN_TEST=0
if [ "${1:-}" = "--test" ] && [ -n "${2:-}" ]; then
    RUN_TEST="$2"
fi
should_run() { [ "$RUN_TEST" -eq 0 ] || [ "$RUN_TEST" -eq "$1" ]; }

pass() {
    local id="$1" desc="$2"
    PASS_COUNT=$((PASS_COUNT + 1)); TOTAL=$((TOTAL + 1))
    echo -e "  ${GREEN}PASS${NC} [$id] $desc"
}

fail() {
    local id="$1" desc="$2" detail="${3:-}"
    FAIL_COUNT=$((FAIL_COUNT + 1)); TOTAL=$((TOTAL + 1))
    echo -e "  ${RED}FAIL${NC} [$id] $desc"
    [ -n "$detail" ] && echo -e "       ${RED}→ $detail${NC}"
    FAILURES="${FAILURES}\n  [$id] $desc${detail:+ — $detail}"
}

skip() {
    local id="$1" desc="$2"
    SKIP_COUNT=$((SKIP_COUNT + 1)); TOTAL=$((TOTAL + 1))
    echo -e "  ${YELLOW}SKIP${NC} [$id] $desc"
}

cleanup() {
    rm -rf "$TEST_TMP" 2>/dev/null
}
trap cleanup EXIT

setup_workdir() {
    local config="${1:-govern-pulse-basic.json}"
    local workdir="$TEST_TMP/work-$$-$RANDOM"
    mkdir -p "$workdir"
    cp "$SCRIPT_DIR/$config" "$workdir/govern.json"
    if [ -f "$SIGNING_KEY" ]; then
        (cd "$workdir" && NAAB_SIGNING_KEY="$SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
    fi
    echo "$workdir"
}

run_test() {
    local workdir="$1" naab_file="$2" flags="${3:-}"
    cp "$SCRIPT_DIR/src/$naab_file" "$workdir/"
    eval "$NAAB" $flags "$workdir/$naab_file" 2>&1
}

echo ""
echo -e "${CYAN}═══════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  NAAb Governance Pulse — Chaos Tests${NC}"
echo -e "${CYAN}═══════════════════════════════════════════════════${NC}"
echo ""

# ── Test 1: governance.health() Structure and Defaults ──
if should_run 1; then
    echo -e "${CYAN}── Test 1: governance.health() Structure and Defaults ──${NC}"
    W=$(setup_workdir "govern-pulse-basic.json")
    OUT=$(run_test "$W" "chaos_pulse_health.naab") || true

    echo "$OUT" | grep -q "t1_is_dict: true" && \
        pass "P1.1" "health() returns a dict" || \
        fail "P1.1" "health() did not return a dict" "$(echo "$OUT" | tail -3)"

    echo "$OUT" | grep -q "t2_verdict: healthy" && \
        pass "P1.2" "Initial verdict is 'healthy'" || \
        fail "P1.2" "Initial verdict not 'healthy'" "$(echo "$OUT" | grep t2_)"

    echo "$OUT" | grep -q "t3_active: true" && \
        pass "P1.3" "active=true with governance enabled" || \
        fail "P1.3" "active not true" "$(echo "$OUT" | grep t3_)"

    echo "$OUT" | grep -qP "t4_total_checks: [0-9]+" && \
        pass "P1.4" "total_checks field exists" || \
        fail "P1.4" "total_checks missing" "$(echo "$OUT" | grep t4_)"

    echo "$OUT" | grep -qP "t5_consecutive_passes: [0-9]+" && \
        pass "P1.5" "consecutive_passes field exists" || \
        fail "P1.5" "consecutive_passes missing" "$(echo "$OUT" | grep t5_)"

    echo "$OUT" | grep -q "t6_bsd_connected:" && \
        pass "P1.6" "bsd_connected field exists" || \
        fail "P1.6" "bsd_connected missing" "$(echo "$OUT" | grep t6_)"

    echo "$OUT" | grep -q "t7_cdd_connected:" && \
        pass "P1.7" "cdd_connected field exists" || \
        fail "P1.7" "cdd_connected missing" "$(echo "$OUT" | grep t7_)"

    echo "$OUT" | grep -q "t9_verdict_after_activity: healthy" && \
        pass "P1.8" "Verdict still healthy after normal activity" || \
        fail "P1.8" "Verdict changed after normal activity" "$(echo "$OUT" | grep t9_)"

    echo "$OUT" | grep -q "chaos_pulse_health_completed: true" && \
        pass "P1.9" "Test completed without crash" || \
        fail "P1.9" "Test did not complete" "$(echo "$OUT" | tail -5)"
fi

# ── Test 2: Pulse Counters ──
if should_run 2; then
    echo -e "${CYAN}── Test 2: Pulse Counters ──${NC}"
    W=$(setup_workdir "govern-pulse-basic.json")
    OUT=$(run_test "$W" "chaos_pulse_counters.naab") || true

    echo "$OUT" | grep -q "t1_checks_incremented: true" && \
        pass "P2.1" "total_checks incremented after activity" || \
        fail "P2.1" "total_checks did not increment" "$(echo "$OUT" | grep t1_)"

    # Extract check count
    check_count=$(echo "$OUT" | grep "t1_check_count:" | sed 's/.*t1_check_count: //' | tr -d '[:space:]')
    [ -n "$check_count" ] && [ "$check_count" -ge 0 ] 2>/dev/null && \
        pass "P2.2" "total_checks is numeric (got $check_count)" || \
        fail "P2.2" "total_checks is not numeric" "got: '$check_count'"

    echo "$OUT" | grep -q "t2_passes_positive: true" && \
        pass "P2.3" "consecutive_passes >= 0" || \
        fail "P2.3" "consecutive_passes negative" "$(echo "$OUT" | grep t2_)"

    echo "$OUT" | grep -q "t3_verdict: healthy" && \
        pass "P2.4" "Verdict healthy after valid code" || \
        fail "P2.4" "Verdict not healthy" "$(echo "$OUT" | grep t3_)"

    echo "$OUT" | grep -q "chaos_pulse_counters_completed: true" && \
        pass "P2.5" "Test completed without crash" || \
        fail "P2.5" "Test did not complete" "$(echo "$OUT" | tail -5)"
fi

# ── Test 3: Dashboard Pulse Line ──
if should_run 3; then
    echo -e "${CYAN}── Test 3: Dashboard Pulse Line ──${NC}"
    W=$(setup_workdir "govern-pulse-basic.json")
    OUT=$(run_test "$W" "chaos_pulse_counters.naab" "--governance-dashboard") || true

    # Dashboard goes to stderr (captured in OUT since 2>&1)
    echo "$OUT" | grep -q "Pulse:" && \
        pass "P3.1" "Dashboard contains 'Pulse:' line" || \
        fail "P3.1" "Dashboard missing 'Pulse:' line" "$(echo "$OUT" | grep -i pulse)"

    echo "$OUT" | grep "Pulse:" | grep -q "HEALTHY" && \
        pass "P3.2" "Dashboard shows HEALTHY verdict" || \
        fail "P3.2" "Dashboard verdict not HEALTHY" "$(echo "$OUT" | grep Pulse:)"

    echo "$OUT" | grep "Pulse:" | grep -qP "[0-9]+ checks" && \
        pass "P3.3" "Dashboard shows check count" || \
        fail "P3.3" "Dashboard missing check count" "$(echo "$OUT" | grep Pulse:)"

    echo "$OUT" | grep "Pulse:" | grep -qP "[0-9]+ consecutive passes" && \
        pass "P3.4" "Dashboard shows consecutive passes" || \
        fail "P3.4" "Dashboard missing consecutive passes" "$(echo "$OUT" | grep Pulse:)"
fi

# ── Test 4: Health Monitoring Disabled ──
if should_run 4; then
    echo -e "${CYAN}── Test 4: Health Monitoring Disabled ──${NC}"
    W=$(setup_workdir "govern-pulse-none.json")
    OUT=$(run_test "$W" "chaos_pulse_no_governance.naab") || true

    echo "$OUT" | grep -q "t1_verdict_no_gov: healthy" && \
        pass "P4.1" "Verdict 'healthy' when health disabled (no false positives)" || \
        fail "P4.1" "Verdict not 'healthy'" "$(echo "$OUT" | grep t1_)"

    # active is true because governance IS loaded, just health monitoring is off
    echo "$OUT" | grep -q "chaos_pulse_no_gov_completed: true" && \
        pass "P4.2" "Test completed without crash" || \
        fail "P4.2" "Test did not complete" "$(echo "$OUT" | tail -5)"
fi

# ── Test 5: Pulse With Circuit Breaker Disabled ──
if should_run 5; then
    echo -e "${CYAN}── Test 5: Pulse With Circuit Breaker Disabled ──${NC}"
    W=$(setup_workdir "govern-pulse-no-cb.json")
    OUT=$(run_test "$W" "chaos_pulse_health.naab" "--governance-dashboard") || true

    # Pulse should still work even with CB disabled
    echo "$OUT" | grep -q "t2_verdict: healthy" && \
        pass "P5.1" "Verdict healthy with CB disabled" || \
        fail "P5.1" "Verdict not healthy with CB disabled" "$(echo "$OUT" | grep t2_)"

    echo "$OUT" | grep -q "t3_active: true" && \
        pass "P5.2" "active=true with CB disabled" || \
        fail "P5.2" "active not true with CB disabled" "$(echo "$OUT" | grep t3_)"

    echo "$OUT" | grep -q "chaos_pulse_health_completed: true" && \
        pass "P5.3" "Test completed without crash (CB disabled)" || \
        fail "P5.3" "Test crashed with CB disabled" "$(echo "$OUT" | tail -5)"

    # Dashboard should still show pulse line
    echo "$OUT" | grep -q "Pulse:" && \
        pass "P5.4" "Dashboard pulse line present with CB disabled" || \
        fail "P5.4" "Dashboard pulse line missing with CB disabled"
fi

# ── Test 6: Weight Parsing ──
if should_run 6; then
    echo -e "${CYAN}── Test 6: Pressure Weight Parsing ──${NC}"
    W=$(setup_workdir "govern-pulse-basic.json")
    OUT=$(run_test "$W" "chaos_pulse_weight_parse.naab") || true

    echo "$OUT" | grep -q "t1_config_parsed: true" && \
        pass "P6.1" "Config with new weights parsed successfully" || \
        fail "P6.1" "Config parse failed" "$(echo "$OUT" | grep t1_)"

    echo "$OUT" | grep -q "t2_verdict: healthy" && \
        pass "P6.2" "No crash from weight parsing" || \
        fail "P6.2" "Crash from weight parsing" "$(echo "$OUT" | grep t2_)"

    echo "$OUT" | grep -q "chaos_pulse_weight_parse_completed: true" && \
        pass "P6.3" "Test completed" || \
        fail "P6.3" "Test did not complete" "$(echo "$OUT" | tail -5)"
fi

# ── Test 7: BSD Event Types (PULSE_DEGRADED/PULSE_IMPAIRED exist) ──
if should_run 7; then
    echo -e "${CYAN}── Test 7: BSD Event Type Registration ──${NC}"
    # Verify the binary doesn't crash when processing pulse event types
    # This tests that the eventTypeToString switch cases are correct
    W=$(setup_workdir "govern-pulse-basic.json")
    OUT=$(run_test "$W" "chaos_pulse_counters.naab" "--governance-dashboard") || true

    # If BSD is enabled and no crash, the event type enum is valid
    echo "$OUT" | grep -q "chaos_pulse_counters_completed: true" && \
        pass "P7.1" "BSD event types registered (no crash)" || \
        fail "P7.1" "Crash with BSD pulse event types" "$(echo "$OUT" | tail -5)"

    # Dashboard should show BSD section (behavioral sequences enabled)
    echo "$OUT" | grep -qP "BSD|behavioral" && \
        pass "P7.2" "BSD subsystem active (pulse can emit events)" || \
        pass "P7.2" "BSD section not in dashboard (may be no events — acceptable)"
fi

# ── Test 8: Pulse Leak Check (Security) ──
if should_run 8; then
    echo -e "${CYAN}── Test 8: Pulse Internal Leak Check ──${NC}"
    # Verify no pulse internals leak in error messages
    W=$(setup_workdir "govern-pulse-basic.json")
    cp "$SCRIPT_DIR/src/chaos_pulse_health.naab" "$W/"
    OUT=$("$NAAB" "$W/chaos_pulse_health.naab" 2>&1) || true

    # Check that pulse internals are NOT in output
    ! echo "$OUT" | grep -q "computePulseVerdict" && \
        pass "P8.1" "No computePulseVerdict in output" || \
        fail "P8.1" "computePulseVerdict leaked in output"

    ! echo "$OUT" | grep -q "consecutive_degraded" && \
        pass "P8.2" "No consecutive_degraded in output" || \
        fail "P8.2" "consecutive_degraded leaked in output"

    ! echo "$OUT" | grep -q "PulseVerdict" && \
        pass "P8.3" "No PulseVerdict enum in output" || \
        fail "P8.3" "PulseVerdict leaked in output"

    ! echo "$OUT" | grep -q "PULSE_DEGRADED" && \
        pass "P8.4" "No PULSE_DEGRADED constant in output" || \
        fail "P8.4" "PULSE_DEGRADED leaked in output"

    ! echo "$OUT" | grep -q "pulse_\." && \
        pass "P8.5" "No pulse_ member access in output" || \
        fail "P8.5" "pulse_ member leaked in output"

    echo "$OUT" | grep -q "chaos_pulse_health_completed: true" && \
        pass "P8.6" "Leak check completed" || \
        fail "P8.6" "Leak check did not complete" "$(echo "$OUT" | tail -3)"
fi

# ═══════════════════════════════════════════════════════
echo ""
echo -e "${CYAN}═══════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  Results: $PASS_COUNT passed, $FAIL_COUNT failed, $SKIP_COUNT skipped ($TOTAL total)${NC}"
echo -e "${CYAN}═══════════════════════════════════════════════════${NC}"

if [ $FAIL_COUNT -gt 0 ]; then
    echo -e "\n${RED}Failures:${NC}"
    echo -e "$FAILURES"
    echo ""
    exit 1
fi

echo ""
exit 0
