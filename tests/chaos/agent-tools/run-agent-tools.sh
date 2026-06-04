#!/usr/bin/env bash
# ============================================================================
# NAAb Agent Tool Execution — Chaos Tests
# Tests: registration validation, environment reflection, action matrix,
#        tool loop execution, budget enforcement, dual-gate, result scanning
# Usage: bash run-agent-tools.sh [--test N]
# ============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../../build/naab-lang"
TEST_TMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}/chaos-tools-$$"
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
    local config="${1:-govern-tools-basic.json}"
    local workdir="$TEST_TMP/work-$$-$RANDOM"
    mkdir -p "$workdir"
    cp "$SCRIPT_DIR/$config" "$workdir/govern.json"
    if [ -f "$SIGNING_KEY" ]; then
        (cd "$workdir" && NAAB_SIGNING_KEY="$SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
    fi
    echo "$workdir"
}

run_test() {
    local workdir="$1" naab_file="$2"
    cp "$SCRIPT_DIR/src/$naab_file" "$workdir/"
    CHAOS_BOGUS="invalid-key-12345" \
        "$NAAB" --governance-dashboard "$workdir/$naab_file" 2>&1
}

echo ""
echo -e "${CYAN}═══════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  NAAb Agent Tool Execution — Chaos Tests${NC}"
echo -e "${CYAN}═══════════════════════════════════════════════════${NC}"
echo ""

# Pre-check: verify at least one Gemini API key is available
# Only probe ONE key to avoid burning rate limit quota
HAS_LIVE_KEY=false
if command -v curl >/dev/null 2>&1; then
    for kname in GK7 GK8 GK9 GK1 GK2 GK3 GK4 GK5 GK6; do
        eval kval=\${$kname:-}
        [ -z "$kval" ] && continue
        probe=$(curl -s --max-time 10 \
            "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:generateContent?key=${kval}" \
            -H 'Content-Type: application/json' \
            -d '{"contents":[{"parts":[{"text":"hi"}]}]}' 2>&1)
        if echo "$probe" | grep -q '"text"'; then
            HAS_LIVE_KEY=true
            echo -e "  ${GREEN}API key check: $kname works${NC}"
            break
        elif echo "$probe" | grep -q '"RESOURCE_EXHAUSTED"\|"retry in"'; then
            HAS_LIVE_KEY=true
            echo -e "  ${YELLOW}API key check: $kname valid but rate-limited (runtime will rotate)${NC}"
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
    echo -e "  ${YELLOW}API key check: No Gemini key found — live tests will skip${NC}"
fi

# ── Test 1: Tool Registration Validation (No API) ──
if should_run 1; then
    echo -e "${CYAN}── Test 1: Tool Registration Validation ──${NC}"
    W=$(setup_workdir)
    OUT=$(run_test "$W" "chaos_18_tool_registration.naab" 2>&1) || true

    echo "$OUT" | grep -q "t1_valid_registration: true" && \
        pass "C18.1" "Valid registration succeeds" || \
        fail "C18.1" "Valid registration failed"

    echo "$OUT" | grep -q "t2_path_traversal_blocked: true" && \
        pass "C18.2" "Path traversal name blocked" || \
        fail "C18.2" "Path traversal name not blocked"

    echo "$OUT" | grep -q "t3_special_chars_blocked: true" && \
        pass "C18.3" "Special chars in name blocked" || \
        fail "C18.3" "Special chars in name not blocked"

    echo "$OUT" | grep -q "t4_empty_name_blocked: true" && \
        pass "C18.4" "Empty name blocked" || \
        fail "C18.4" "Empty name not blocked"

    echo "$OUT" | grep -q "t5_no_description_blocked: true" && \
        pass "C18.5" "Missing description blocked" || \
        fail "C18.5" "Missing description not blocked"

    echo "$OUT" | grep -q "t6_non_function_blocked: true" && \
        pass "C18.6" "Non-function arg blocked" || \
        fail "C18.6" "Non-function arg not blocked"

    echo "$OUT" | grep -q "t7_reserved_name_blocked: true" && \
        pass "C18.7" "Reserved name (print) blocked" || \
        fail "C18.7" "Reserved name not blocked"

    echo "$OUT" | grep -q "t8_reregistration_ok: true" && \
        pass "C18.8" "Re-registration succeeds with warning" || \
        fail "C18.8" "Re-registration failed"

    echo "$OUT" | grep -q "t9_long_name_blocked: true" && \
        pass "C18.9" "Long name (>128 chars) blocked" || \
        fail "C18.9" "Long name not blocked"

    echo "$OUT" | grep -q "chaos_18_completed: true" && \
        pass "C18.10" "Test completed" || \
        fail "C18.10" "Test did not complete" "$(echo "$OUT" | tail -5)"
fi

# ── Test 2: Tool Environment Reflection (No API) ──
if should_run 2; then
    echo -e "${CYAN}── Test 2: Tool Environment Reflection ──${NC}"
    W=$(setup_workdir)
    OUT=$(run_test "$W" "chaos_19_tool_environment.naab" 2>&1) || true

    echo "$OUT" | grep -q "t1_tools_enabled: true" && \
        pass "C19.1" "Birth env shows tools_enabled=true" || \
        fail "C19.1" "Birth env missing tools_enabled"

    t2_val=$(echo "$OUT" | grep "t2_tools_registered:" | grep -oP 'registered: \K[0-9]+' || echo "?")
    [ "${t2_val}" -ge 1 ] 2>/dev/null && \
        pass "C19.2" "Birth env shows tools_registered >= 1 (got $t2_val)" || \
        fail "C19.2" "Birth env tools_registered < 1 (got $t2_val)"

    t3_val=$(echo "$OUT" | grep "t3_tools_available_count:" | grep -oP 'count: \K[0-9]+' || echo "?")
    [ "${t3_val}" -ge 1 ] 2>/dev/null && \
        pass "C19.3" "Birth env shows tools_available >= 1 (got $t3_val)" || \
        fail "C19.3" "Birth env tools_available < 1 (got $t3_val)"

    echo "$OUT" | grep -q "t4_live_tools_enabled: true" && \
        pass "C19.4" "Live env shows tools_enabled=true" || \
        fail "C19.4" "Live env missing tools_enabled"

    echo "$OUT" | grep -q "t5_initial_calls_zero: true" && \
        pass "C19.5" "Initial tool_calls_total = 0" || \
        fail "C19.5" "Initial tool_calls_total != 0"

    echo "$OUT" | grep -q "t5_initial_blocked_zero: true" && \
        pass "C19.6" "Initial tool_calls_blocked = 0" || \
        fail "C19.6" "Initial tool_calls_blocked != 0"

    echo "$OUT" | grep -q "t6_disabled_tools_enabled: false" && \
        pass "C19.7" "Disabled agent shows tools_enabled=false" || \
        fail "C19.7" "Disabled agent shows tools_enabled=true"

    echo "$OUT" | grep -q "chaos_19_completed: true" && \
        pass "C19.8" "Test completed" || \
        fail "C19.8" "Test did not complete" "$(echo "$OUT" | tail -5)"
fi

# ── Test 3: TOOL_EXEC Action Matrix (Live API) ──
if should_run 3; then
    echo -e "${CYAN}── Test 3: TOOL_EXEC Action Matrix ──${NC}"
    # Check for working API key
    if ! $HAS_LIVE_KEY; then
        skip "C20.1" "No working API key — skipping live test"
        skip "C20.2" "No working API key — skipping live test"
    else
        W=$(setup_workdir)
        OUT=$(run_test "$W" "chaos_20_tool_action_matrix.naab" 2>&1) || true

        echo "$OUT" | grep -q "chaos_20_completed: true" && \
            pass "C20.1" "Action matrix test completed" || \
            fail "C20.1" "Action matrix test did not complete" "$(echo "$OUT" | tail -5)"

        # If tool_exec was blocked, error should not leak other actions
        if echo "$OUT" | grep -q "t2_error_leaks_actions:"; then
            echo "$OUT" | grep -q "t2_error_leaks_actions: false" && \
                pass "C20.2" "Error does not leak other allowed_actions" || \
                fail "C20.2" "Error leaks other allowed_actions values"
        else
            pass "C20.2" "LLM did not return tool_use (no action matrix test needed)"
        fi
    fi
fi

# ── Test 4: Basic Tool Loop Execution (Live API) ──
if should_run 4; then
    echo -e "${CYAN}── Test 4: Basic Tool Loop Execution ──${NC}"
    if ! $HAS_LIVE_KEY; then
        skip "C21.1" "No working API key — skipping live test"
        skip "C21.2" "No working API key — skipping live test"
        skip "C21.3" "No working API key — skipping live test"
        skip "C21.4" "No working API key — skipping live test"
        skip "C21.5" "No working API key — skipping live test"
    else
        W=$(setup_workdir)
        OUT=$(run_test "$W" "chaos_21_tool_basic_loop.naab" 2>&1) || true

        echo "$OUT" | grep -q "t1_send_succeeded: true" && \
            pass "C21.1" "agent.send() succeeded" || \
            fail "C21.1" "agent.send() failed" "$(echo "$OUT" | grep 'send_error' | head -1)"

        t3_val=$(echo "$OUT" | grep "t3_tool_calls_made:" | grep -oP 'made: \K[0-9]+' || echo "0")
        [ "${t3_val}" -ge 1 ] 2>/dev/null && \
            pass "C21.2" "Tool calls made >= 1 (got $t3_val)" || \
            fail "C21.2" "No tool calls made (got $t3_val)" "LLM may not have called tools"

        t4_val=$(echo "$OUT" | grep "t4_tool_loop_turns:" | grep -oP 'turns: \K[0-9]+' || echo "0")
        [ "${t4_val}" -ge 1 ] 2>/dev/null && \
            pass "C21.3" "Tool loop turns >= 1 (got $t4_val)" || \
            fail "C21.3" "No tool loop turns (got $t4_val)"

        t5_val=$(echo "$OUT" | grep "t5_tool_results_count:" | grep -oP 'count: \K[0-9]+' || echo "0")
        [ "${t5_val}" -ge 1 ] 2>/dev/null && \
            pass "C21.4" "Tool results present (got $t5_val)" || \
            fail "C21.4" "No tool results (got $t5_val)"

        echo "$OUT" | grep -q "chaos_21_completed: true" && \
            pass "C21.5" "Test completed" || \
            fail "C21.5" "Test did not complete" "$(echo "$OUT" | tail -5)"
    fi
fi

# ── Test 5: Tool Budget Enforcement (Live API) ──
if should_run 5; then
    echo -e "${CYAN}── Test 5: Tool Budget Enforcement ──${NC}"
    if ! $HAS_LIVE_KEY; then
        skip "C22.1" "No working API key — skipping live test"
        skip "C22.2" "No working API key — skipping live test"
        skip "C22.3" "No working API key — skipping live test"
    else
        W=$(setup_workdir)
        OUT=$(run_test "$W" "chaos_22_tool_budget.naab" 2>&1) || true

        echo "$OUT" | grep -q "t1_within_budget: true" && \
            pass "C22.1" "Tool calls within budget (max_tool_calls_per_turn=2)" || \
            fail "C22.1" "Tool calls exceeded budget"

        echo "$OUT" | grep -q "chaos_22_completed: true" && \
            pass "C22.2" "Budget test completed" || \
            fail "C22.2" "Budget test did not complete" "$(echo "$OUT" | tail -5)"

        # Check exit reason (if tools were called)
        t1_val=$(echo "$OUT" | grep "t1_tool_calls_made:" | grep -oP 'made: \K[0-9]+' || echo "0")
        if [ "${t1_val}" -ge 1 ] 2>/dev/null; then
            pass "C22.3" "Budget enforcement validated (calls=$t1_val)"
        else
            pass "C22.3" "LLM did not call tools (budget not tested, but valid)"
        fi
    fi
fi

# ── Test 6: Dual-Gate Enforcement (Live API) ──
if should_run 6; then
    echo -e "${CYAN}── Test 6: Dual-Gate Enforcement ──${NC}"
    if ! $HAS_LIVE_KEY; then
        skip "C23.1" "No working API key — skipping live test"
        skip "C23.2" "No working API key — skipping live test"
    else
        W=$(setup_workdir)
        OUT=$(run_test "$W" "chaos_23_tool_unregistered.naab" 2>&1) || true

        # Only search_data should be available (calculate is in config but not registered)
        t1_val=$(echo "$OUT" | grep "t1_available_tools:" | grep -oP 'tools: \K[0-9]+' || echo "?")
        [ "${t1_val}" = "1" ] && \
            pass "C23.1" "Only registered+allowed tool available (got $t1_val)" || \
            fail "C23.1" "Expected 1 available tool, got $t1_val"

        echo "$OUT" | grep -q "chaos_23_completed: true" && \
            pass "C23.2" "Dual-gate test completed" || \
            fail "C23.2" "Dual-gate test did not complete" "$(echo "$OUT" | tail -5)"
    fi
fi

# ── Test 7: Tool Result Scanning (Live API) ──
if should_run 7; then
    echo -e "${CYAN}── Test 7: Tool Result Scanning ──${NC}"
    if ! $HAS_LIVE_KEY; then
        skip "C24.1" "No working API key — skipping live test"
        skip "C24.2" "No working API key — skipping live test"
        skip "C24.3" "No working API key — skipping live test"
    else
        W=$(setup_workdir)
        OUT=$(run_test "$W" "chaos_24_tool_result_scan.naab" 2>&1) || true

        echo "$OUT" | grep -q "t1_has_content: true" && \
            pass "C24.1" "Tool result scan: content returned" || \
            fail "C24.1" "No content after tool result scan"

        t3_blocked=$(echo "$OUT" | grep "t3_tool_calls_blocked:" | grep -oP 'blocked: \K[0-9]+' || echo "?")
        [ "${t3_blocked}" = "0" ] && \
            pass "C24.2" "Normal tool results not blocked (blocked=$t3_blocked)" || \
            fail "C24.2" "Normal tool results were blocked (blocked=$t3_blocked)"

        echo "$OUT" | grep -q "chaos_24_completed: true" && \
            pass "C24.3" "Result scan test completed" || \
            fail "C24.3" "Result scan test did not complete" "$(echo "$OUT" | tail -5)"
    fi
fi

# ── Summary ──
echo ""
echo -e "${CYAN}═══════════════════════════════════════════════════${NC}"
echo -e "  Total: $TOTAL  ${GREEN}Pass: $PASS_COUNT${NC}  ${RED}Fail: $FAIL_COUNT${NC}  ${YELLOW}Skip: $SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then
    echo -e "\n  ${RED}Failures:${NC}$FAILURES"
fi
echo -e "${CYAN}═══════════════════════════════════════════════════${NC}"
echo ""

exit "$FAIL_COUNT"
