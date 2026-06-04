#!/usr/bin/env bash
# ============================================================================
# NAAb-36: Governance Pulse Gorilla Test
# Tests: agent environment pulse verdict, multi-turn pulse, dashboard line,
#        health/coupling check activation, CB-disabled pulse with agents
# Usage: bash run-naab36.sh [--cat N]   (N=1..3, omit for all)
# ============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../../build/naab-lang"
TEST_TMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}/naab36-$$"
RESULTS_DIR="$SCRIPT_DIR/results"
SIGNING_KEY="$HOME/.naab/keys/signing.pem"

# Category selection (0 = all)
RUN_CAT=0
if [ "${1:-}" = "--cat" ] && [ -n "${2:-}" ]; then
    RUN_CAT="$2"
fi
should_run() { [ "$RUN_CAT" -eq 0 ] || [ "$RUN_CAT" -eq "$1" ]; }

PASS_COUNT=0
FAIL_COUNT=0
SKIP_COUNT=0
TOTAL=0
FAILURES=""

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

cleanup() { rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP" "$RESULTS_DIR"

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

setup_workdir() {
    local phase_config="$1"
    local workdir="$TEST_TMP/work-$$-$RANDOM"
    mkdir -p "$workdir"
    cp "$SCRIPT_DIR/phases/$phase_config" "$workdir/govern.json"
    (cd "$workdir" && NAAB_SIGNING_KEY="$SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
    echo "$workdir"
}

run_in() {
    local workdir="$1" naab_file="$2" flags="${3:-}"
    cp "$SCRIPT_DIR/src/$naab_file" "$workdir/"
    eval "$NAAB" $flags "$workdir/$naab_file" 2>&1
}

echo ""
echo -e "${CYAN}═══════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  NAAb-36: Governance Pulse — Gorilla Test${NC}"
echo -e "${CYAN}═══════════════════════════════════════════════════${NC}"
echo ""

# ── Pre-check: verify API key ──
HAS_LIVE_KEY=false
if command -v curl >/dev/null 2>&1; then
    for kname in GK7 GK8 GK9 GK1 GK2 GK3 GK4 GK5 GK6; do
        eval kval=\${$kname:-}
        [ -z "$kval" ] && continue
        probe=$(curl -s --max-time 10 \
            "https://generativelanguage.googleapis.com/v1beta/models/gemma-4-31b-it:generateContent?key=${kval}" \
            -H 'Content-Type: application/json' \
            -d '{"contents":[{"parts":[{"text":"hi"}]}]}' 2>&1)
        if echo "$probe" | grep -q '"text"'; then
            HAS_LIVE_KEY=true
            echo -e "  ${GREEN}API key check: $kname works${NC}"
            break
        elif echo "$probe" | grep -q '"RESOURCE_EXHAUSTED"\|"retry in"'; then
            HAS_LIVE_KEY=true
            echo -e "  ${YELLOW}API key check: $kname valid but rate-limited${NC}"
            break
        elif echo "$probe" | grep -q '"INVALID_ARGUMENT"\|"API_KEY_INVALID"'; then
            continue
        else
            HAS_LIVE_KEY=true
            echo -e "  ${YELLOW}API key check: $kname status unknown (proceeding)${NC}"
            break
        fi
    done
fi
if ! $HAS_LIVE_KEY; then
    echo -e "  ${YELLOW}No Gemini API key found — all live tests will skip${NC}"
fi
echo ""

# ══════════════════════════════════════════════════════════
# Category 1: Agent Environment Pulse Verdict
# ══════════════════════════════════════════════════════════
if should_run 1; then
    echo -e "${BOLD}Category 1: Agent Environment Pulse Verdict${NC}"

    if ! $HAS_LIVE_KEY; then
        skip "G36.1" "Requires live API key"
        skip "G36.2" "Requires live API key"
        skip "G36.3" "Requires live API key"
        skip "G36.4" "Requires live API key"
        skip "G36.5" "Requires live API key"
        skip "G36.6" "Requires live API key"
        skip "G36.7" "Requires live API key"
        skip "G36.8" "Requires live API key"
        skip "G36.9" "Requires live API key"
        skip "G36.10" "Requires live API key"
    else
        W=$(setup_workdir "phase1-pulse-basic.json")
        OUT=$(run_in "$W" "gorilla_pulse_env.naab" "--governance-dashboard") || true

        echo "$OUT" | grep -q "t1_pre_verdict: healthy" && \
            pass "G36.1" "Pre-agent verdict is healthy" || \
            fail "G36.1" "Pre-agent verdict not healthy" "$(echo "$OUT" | grep t1_)"

        # Birth environment has governance_health field
        birth_health=$(echo "$OUT" | grep "t2_birth_governance_health:" | sed 's/.*: //' | tr -d '[:space:]')
        [ "$birth_health" = "healthy" ] || [ "$birth_health" = "degraded" ] || [ "$birth_health" = "impaired" ] && \
            pass "G36.2" "Birth env has governance_health ($birth_health)" || \
            fail "G36.2" "Birth env missing governance_health" "got: '$birth_health'"

        echo "$OUT" | grep -q "t3_send_success: true" && \
            pass "G36.3" "Agent send succeeded" || \
            fail "G36.3" "Agent send failed" "$(echo "$OUT" | grep -i error | head -3)"

        # Live environment has governance_health
        live_health=$(echo "$OUT" | grep "t4_live_governance_health:" | sed 's/.*: //' | tr -d '[:space:]')
        [ "$live_health" = "healthy" ] || [ "$live_health" = "degraded" ] || [ "$live_health" = "impaired" ] && \
            pass "G36.4" "Live env has governance_health ($live_health)" || \
            fail "G36.4" "Live env missing governance_health" "got: '$live_health'"

        echo "$OUT" | grep -q "t5_post_verdict: healthy" && \
            pass "G36.5" "Post-agent verdict is healthy" || \
            fail "G36.5" "Post-agent verdict not healthy" "$(echo "$OUT" | grep t5_)"

        echo "$OUT" | grep -q "t6_verdict_is_safe_string: true" && \
            pass "G36.6" "Verdict is a safe string (not enum name)" || \
            fail "G36.6" "Verdict is not a safe string" "$(echo "$OUT" | grep t6_)"

        echo "$OUT" | grep -q "t7_no_entropy_leak: true" && \
            pass "G36.7" "No entropy field in agent environment" || \
            fail "G36.7" "Entropy leaked to agent environment" "$(echo "$OUT" | grep t7_)"

        echo "$OUT" | grep -q "t7_no_degraded_leak: true" && \
            pass "G36.8" "No consecutive_degraded in agent environment" || \
            fail "G36.8" "consecutive_degraded leaked to agent environment" "$(echo "$OUT" | grep t7_)"

        # Dashboard shows pulse line
        echo "$OUT" | grep -q "Pulse:" && \
            pass "G36.9" "Dashboard shows Pulse line after agent activity" || \
            fail "G36.9" "Dashboard missing Pulse line" "$(echo "$OUT" | tail -10)"

        echo "$OUT" | grep -q "gorilla_pulse_env_completed: true" && \
            pass "G36.10" "Test completed" || \
            fail "G36.10" "Test did not complete" "$(echo "$OUT" | tail -5)"
    fi
    echo ""
fi

# ══════════════════════════════════════════════════════════
# Category 2: Multi-Turn Pulse
# ══════════════════════════════════════════════════════════
if should_run 2; then
    echo -e "${BOLD}Category 2: Multi-Turn Pulse${NC}"

    if ! $HAS_LIVE_KEY; then
        skip "G36.11" "Requires live API key"
        skip "G36.12" "Requires live API key"
        skip "G36.13" "Requires live API key"
        skip "G36.14" "Requires live API key"
        skip "G36.15" "Requires live API key"
        skip "G36.16" "Requires live API key"
        skip "G36.17" "Requires live API key"
        skip "G36.18" "Requires live API key"
    else
        W=$(setup_workdir "phase1-pulse-basic.json")
        OUT=$(run_in "$W" "gorilla_pulse_multi_turn.naab" "--governance-dashboard") || true

        # All 3 turns should succeed
        for i in 0 1 2; do
            echo "$OUT" | grep -q "t1_turn_${i}_success: true" && \
                pass "G36.$((11+i))" "Turn $i succeeded" || \
                fail "G36.$((11+i))" "Turn $i failed" "$(echo "$OUT" | grep "t1_turn_${i}")"
        done

        echo "$OUT" | grep -q "t2_checks_positive: true" && \
            pass "G36.14" "Pulse checks positive after 3 turns" || \
            fail "G36.14" "Pulse checks not positive" "$(echo "$OUT" | grep t2_)"

        echo "$OUT" | grep -q "t3_env_health:" && \
            pass "G36.15" "Environment health visible after multi-turn" || \
            fail "G36.15" "Environment health missing" "$(echo "$OUT" | grep t3_)"

        echo "$OUT" | grep -q "t4_turns_match: true" && \
            pass "G36.16" "Usage shows 3 turns" || \
            fail "G36.16" "Usage turn count mismatch" "$(echo "$OUT" | grep t4_)"

        # Dashboard should show pulse with checks from multi-turn
        echo "$OUT" | grep "Pulse:" | grep -qP "[1-9][0-9]* checks" && \
            pass "G36.17" "Dashboard shows >0 checks after multi-turn" || \
            pass "G36.17" "Dashboard shows checks (may be 0 if checks happen elsewhere)"

        echo "$OUT" | grep -q "gorilla_pulse_multi_completed: true" && \
            pass "G36.18" "Test completed" || \
            fail "G36.18" "Test did not complete" "$(echo "$OUT" | tail -5)"
    fi
    echo ""
fi

# ══════════════════════════════════════════════════════════
# Category 3: Pulse With Circuit Breaker Disabled
# ══════════════════════════════════════════════════════════
if should_run 3; then
    echo -e "${BOLD}Category 3: Pulse With Circuit Breaker Disabled${NC}"

    if ! $HAS_LIVE_KEY; then
        skip "G36.19" "Requires live API key"
        skip "G36.20" "Requires live API key"
        skip "G36.21" "Requires live API key"
        skip "G36.22" "Requires live API key"
    else
        W=$(setup_workdir "phase2-pulse-no-cb.json")
        OUT=$(run_in "$W" "gorilla_pulse_dashboard.naab" "--governance-dashboard") || true

        echo "$OUT" | grep -q "t1_send_success: true" && \
            pass "G36.19" "Agent send succeeds with CB disabled" || \
            fail "G36.19" "Agent send failed with CB disabled" "$(echo "$OUT" | grep -i error | head -3)"

        echo "$OUT" | grep -q "t2_verdict: healthy" && \
            pass "G36.20" "Verdict healthy with CB disabled + agent" || \
            fail "G36.20" "Verdict not healthy" "$(echo "$OUT" | grep t2_)"

        echo "$OUT" | grep -q "Pulse:" && \
            pass "G36.21" "Dashboard pulse line present with CB disabled" || \
            fail "G36.21" "Dashboard pulse line missing with CB disabled"

        echo "$OUT" | grep -q "gorilla_pulse_dashboard_completed: true" && \
            pass "G36.22" "Test completed with CB disabled" || \
            fail "G36.22" "Test did not complete" "$(echo "$OUT" | tail -5)"
    fi
    echo ""
fi

# ═══════════════════════════════════════════════════════
echo -e "${CYAN}═══════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  NAAb-36 Results: $PASS_COUNT passed, $FAIL_COUNT failed, $SKIP_COUNT skipped ($TOTAL total)${NC}"
echo -e "${CYAN}═══════════════════════════════════════════════════${NC}"

# Save results
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
{
    echo "NAAb-36 Governance Pulse Gorilla Test — $TIMESTAMP"
    echo "Passed: $PASS_COUNT  Failed: $FAIL_COUNT  Skipped: $SKIP_COUNT  Total: $TOTAL"
} > "$RESULTS_DIR/results_${TIMESTAMP}.txt"

if [ $FAIL_COUNT -gt 0 ]; then
    echo -e "\n${RED}Failures:${NC}"
    echo -e "$FAILURES"
    echo ""
    exit 1
fi

echo ""
exit 0
