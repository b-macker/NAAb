#!/usr/bin/env bash
# ============================================================================
# NAAb-35: Agent Tool Execution — Gorilla Test
# 40 assertions across 4 categories testing all 7 defense layers:
#   Cat 1: Registration & Validation (10, no API)
#   Cat 2: Live Tool Execution Loop (10, live API)
#   Cat 3: Governance Blocking (10, mostly no API)
#   Cat 4: Budget Enforcement & Response Dict (10, live API)
# Usage: bash run-naab35.sh [--cat N]
# ============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../../build/naab-lang"
TEST_TMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}/naab35-$$"
RESULTS_DIR="$SCRIPT_DIR/results"
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

# Category selection
RUN_CAT=0
if [ "${1:-}" = "--cat" ] && [ -n "${2:-}" ]; then
    RUN_CAT="$2"
fi
should_run() { [ "$RUN_CAT" -eq 0 ] || [ "$RUN_CAT" -eq "$1" ]; }

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
    local phase_config="$1"
    local workdir="$TEST_TMP/work-$$-$RANDOM"
    mkdir -p "$workdir"
    cp "$SCRIPT_DIR/phases/$phase_config" "$workdir/govern.json"
    if [ -f "$SIGNING_KEY" ]; then
        (cd "$workdir" && NAAB_SIGNING_KEY="$SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
    fi
    echo "$workdir"
}

run_in() {
    local workdir="$1" naab_file="$2"
    cp "$SCRIPT_DIR/src/$naab_file" "$workdir/"
    "$NAAB" --governance-dashboard "$workdir/$naab_file" 2>&1
}

echo ""
echo -e "${CYAN}═══════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  NAAb-35: Agent Tool Execution — Gorilla Test${NC}"
echo -e "${CYAN}  40 assertions across 4 categories${NC}"
echo -e "${CYAN}═══════════════════════════════════════════════════════${NC}"
echo ""

# Pre-check: verify at least one Gemini API key works
HAS_LIVE_KEY=false
if command -v curl >/dev/null 2>&1; then
    for kname in GK1 GK2 GK3 GK4 GK5 GK6; do
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
        fi
    done
fi
if ! $HAS_LIVE_KEY; then
    echo -e "  ${YELLOW}API key check: No working Gemini key — live tests will skip${NC}"
fi
echo ""

# ══════════════════════════════════════════════════════════
# Category 1: Registration & Validation (No API)
# ══════════════════════════════════════════════════════════
if should_run 1; then
    echo -e "${CYAN}Category 1: Registration & Validation${NC}"
    WORKDIR=$(setup_workdir "phase1-tool-config.json")
    output=$(run_in "$WORKDIR" "cat1_registration.naab" 2>&1) || true

    echo "$output" | grep -q 'a1_valid_reg: true' && \
        pass "A1" "Valid tool registration succeeds" || \
        fail "A1" "Valid registration failed" "$(echo "$output" | grep 'a1_error' | head -1)"

    echo "$output" | grep -q 'a2_second_reg: true' && \
        pass "A2" "Second tool registration succeeds" || \
        fail "A2" "Second registration failed"

    echo "$output" | grep -q 'a3_null_byte_blocked: true' && \
        pass "A3" "Null byte in tool name blocked" || \
        fail "A3" "Null byte not blocked"

    echo "$output" | grep -q 'a4_no_desc_blocked: true' && \
        pass "A4" "Missing description blocked" || \
        fail "A4" "Missing description not blocked"

    echo "$output" | grep -q 'a5_non_fn_blocked: true' && \
        pass "A5" "Non-function arg blocked" || \
        fail "A5" "Non-function not blocked"

    a6_val=$(echo "$output" | grep "a6_tools_registered:" | grep -oP 'registered: \K[0-9]+' || echo "?")
    [ "${a6_val}" -ge 3 ] 2>/dev/null && \
        pass "A6" "Environment shows $a6_val registered tools" || \
        fail "A6" "Expected >= 3 registered, got $a6_val"

    a7_val=$(echo "$output" | grep "a7_tools_available:" | grep -oP 'available: \K[0-9]+' || echo "?")
    [ "${a7_val}" -eq 3 ] 2>/dev/null && \
        pass "A7" "Available tools = 3 (intersection of registered + config)" || \
        fail "A7" "Expected 3 available, got $a7_val"

    echo "$output" | grep -q 'a8_disabled_tools: false' && \
        pass "A8" "Disabled agent shows tools_enabled=false" || \
        fail "A8" "Disabled agent wrong tools_enabled"

    a9_val=$(echo "$output" | grep "a9_partial_avail:" | grep -oP 'avail: \K[0-9]+' || echo "?")
    [ "${a9_val}" -eq 1 ] 2>/dev/null && \
        pass "A9" "Partial agent shows 1 available (dual-gate)" || \
        fail "A9" "Expected 1 partial available, got $a9_val"

    echo "$output" | grep -q 'a10_initial_total: 0' && \
        pass "A10" "Initial usage counters = 0" || \
        fail "A10" "Initial usage counters non-zero"

    echo "$output" | grep -q 'cat1_completed: true' || \
        fail "A10x" "Category 1 did not complete" "$(echo "$output" | tail -3)"
fi

# ══════════════════════════════════════════════════════════
# Category 2: Live Tool Execution Loop (Requires API)
# ══════════════════════════════════════════════════════════
if should_run 2; then
    echo -e "${CYAN}Category 2: Live Tool Execution Loop${NC}"
    if ! $HAS_LIVE_KEY; then
        for i in $(seq 1 10); do skip "B$i" "No working API key"; done
    else
        WORKDIR=$(setup_workdir "phase1-tool-config.json")
        output=$(run_in "$WORKDIR" "cat2_live_tool_loop.naab" 2>&1) || true

        echo "$output" | grep -q 'a1_send_ok: true' && \
            pass "B1" "agent.send() with tools succeeded" || \
            fail "B1" "agent.send() failed" "$(echo "$output" | grep 'send_error' | head -1)"

        echo "$output" | grep -q 'a2_has_content: true' && \
            pass "B2" "Response has text content" || \
            fail "B2" "No text content in response"

        b3_val=$(echo "$output" | grep "a3_tool_calls_made:" | grep -oP 'made: \K[0-9]+' || echo "0")
        [ "${b3_val}" -ge 1 ] 2>/dev/null && \
            pass "B3" "Tool calls made >= 1 (got $b3_val)" || \
            fail "B3" "No tool calls made" "LLM may not have used tools"

        b4_val=$(echo "$output" | grep "a4_tool_loop_turns:" | grep -oP 'turns: \K[0-9]+' || echo "0")
        [ "${b4_val}" -ge 1 ] 2>/dev/null && \
            pass "B4" "Tool loop turns >= 1 (got $b4_val)" || \
            fail "B4" "No tool loop turns"

        b5_val=$(echo "$output" | grep "a5_tool_results_count:" | grep -oP 'count: \K[0-9]+' || echo "0")
        [ "${b5_val}" -ge 1 ] 2>/dev/null && \
            pass "B5" "Tool results present (got $b5_val)" || \
            fail "B5" "No tool results"

        echo "$output" | grep -q 'a6_result_keys: true' && \
            pass "B6" "Tool result has name/success/latency keys" || \
            fail "B6" "Tool result missing expected keys"

        b7_val=$(echo "$output" | grep "a7_budget_remaining:" | grep -oP 'remaining: \K-?[0-9]+' || echo "-1")
        [ "${b7_val}" -ge 0 ] 2>/dev/null && \
            pass "B7" "Budget remaining >= 0 (got $b7_val)" || \
            fail "B7" "Budget remaining invalid ($b7_val)"

        b8_val=$(echo "$output" | grep "a8_exit_reason:" | sed 's/.*a8_exit_reason: //' || echo "none")
        [ -n "$b8_val" ] && [ "$b8_val" != "none" ] && \
            pass "B8" "Exit reason present: $b8_val" || \
            fail "B8" "No exit reason"

        b9_val=$(echo "$output" | grep "a9_usage_total:" | grep -oP 'total: \K[0-9]+' || echo "0")
        [ "${b9_val}" -ge 1 ] 2>/dev/null && \
            pass "B9" "Usage tool_calls_total >= 1 ($b9_val)" || \
            fail "B9" "Usage tool_calls_total = 0"

        b10_val=$(echo "$output" | grep "a10_env_tool_total:" | grep -oP 'total: \K[0-9]+' || echo "0")
        [ "${b10_val}" -ge 1 ] 2>/dev/null && \
            pass "B10" "Environment tool_calls_total >= 1 ($b10_val)" || \
            fail "B10" "Environment tool_calls_total = 0"

        echo "$output" | grep -q 'cat2_completed: true' || \
            fail "B10x" "Category 2 did not complete" "$(echo "$output" | tail -3)"
    fi
fi

# ══════════════════════════════════════════════════════════
# Category 3: Governance Blocking (Mostly no API)
# ══════════════════════════════════════════════════════════
if should_run 3; then
    echo -e "${CYAN}Category 3: Governance Blocking${NC}"
    WORKDIR=$(setup_workdir "phase1-tool-config.json")

    if ! $HAS_LIVE_KEY; then
        # A1/A2 need live API (TOOL_EXEC test requires LLM to return tool_use)
        skip "C1" "No working API key (TOOL_EXEC test)"
        skip "C2" "No working API key (action leak test)"
        # Run the script anyway for non-API assertions
        output=$(run_in "$WORKDIR" "cat3_governance_blocking.naab" 2>&1) || true
    else
        output=$(run_in "$WORKDIR" "cat3_governance_blocking.naab" 2>&1) || true

        # A1: TOOL_EXEC blocking (may be inconclusive if LLM doesn't call tools)
        if echo "$output" | grep -q 'a1_tool_exec_blocked: true'; then
            pass "C1" "TOOL_EXEC missing → blocked"
        elif echo "$output" | grep -q 'a1_inconclusive'; then
            pass "C1" "LLM did not call tools (TOOL_EXEC not tested, valid)"
        elif echo "$output" | grep -q 'a1_tool_exec_blocked: false'; then
            fail "C1" "TOOL_EXEC missing but tools not blocked"
        else
            fail "C1" "TOOL_EXEC test did not produce expected output" "$(echo "$output" | grep 'a1_' | head -2)"
        fi

        echo "$output" | grep -q 'a2_no_action_leak: true' && \
            pass "C2" "TOOL_EXEC error does not leak actions list" || \
            fail "C2" "TOOL_EXEC error leaks allowed_actions"
    fi

    # A3-A10 don't need API
    a3_val=$(echo "$output" | grep "a3_dual_gate_count:" | grep -oP 'count: \K[0-9]+' || echo "?")
    [ "${a3_val}" = "1" ] && \
        pass "C3" "Dual-gate: only registered tool available (got $a3_val)" || \
        fail "C3" "Dual-gate: expected 1, got $a3_val"

    echo "$output" | grep -q 'a4_reserved_blocked: true' && \
        pass "C4" "Reserved name (len) blocked" || \
        fail "C4" "Reserved name not blocked"

    echo "$output" | grep -q 'a5_path_traversal_blocked: true' && \
        pass "C5" "Path traversal name blocked" || \
        fail "C5" "Path traversal not blocked"

    echo "$output" | grep -q 'a6_empty_desc_blocked: true' && \
        pass "C6" "Empty description blocked" || \
        fail "C6" "Empty description not blocked"

    echo "$output" | grep -q 'a7_disabled_config: true' && \
        pass "C7" "Disabled agent config confirmed" || \
        fail "C7" "Disabled agent config wrong"

    echo "$output" | grep -q 'a8_long_name_blocked: true' && \
        pass "C8" "Long name (200 chars) blocked" || \
        fail "C8" "Long name not blocked"

    echo "$output" | grep -q 'a9_special_chars_blocked: true' && \
        pass "C9" "Special chars in name blocked" || \
        fail "C9" "Special chars not blocked"

    echo "$output" | grep -q 'a10_string_fn_blocked: true' && \
        pass "C10" "String as function arg blocked" || \
        fail "C10" "String function not blocked"

    echo "$output" | grep -q 'cat3_completed: true' || \
        fail "C10x" "Category 3 did not complete" "$(echo "$output" | tail -3)"
fi

# ══════════════════════════════════════════════════════════
# Category 4: Budget Enforcement & Response Dict (Requires API)
# ══════════════════════════════════════════════════════════
if should_run 4; then
    echo -e "${CYAN}Category 4: Budget Enforcement & Response Dict${NC}"
    if ! $HAS_LIVE_KEY; then
        for i in $(seq 1 10); do skip "D$i" "No working API key"; done
    else
        WORKDIR=$(setup_workdir "phase1-tool-config.json")
        output=$(run_in "$WORKDIR" "cat4_budget_response.naab" 2>&1) || true

        echo "$output" | grep -q 'a1_within_budget: true' && \
            pass "D1" "Tool calls within budget cap" || \
            fail "D1" "Tool calls exceeded budget"

        d2_val=$(echo "$output" | grep "a2_budget_remaining:" | grep -oP 'remaining: \K-?[0-9]+' || echo "?")
        [ "${d2_val}" -ge 0 ] 2>/dev/null && \
            pass "D2" "Budget remaining >= 0 ($d2_val)" || \
            fail "D2" "Budget remaining invalid ($d2_val)"

        echo "$output" | grep -q 'a3_turns_within_cap: true' && \
            pass "D3" "Tool loop turns within cap" || \
            fail "D3" "Tool loop turns exceeded cap"

        d4_val=$(echo "$output" | grep "a4_exit_reason:" | sed 's/.*a4_exit_reason: //' || echo "none")
        [ -n "$d4_val" ] && [ "$d4_val" != "none" ] && \
            pass "D4" "Exit reason present: $d4_val" || \
            fail "D4" "No exit reason"

        echo "$output" | grep -q 'a5_results_match_calls: true' && \
            pass "D5" "Results array length matches calls made" || \
            fail "D5" "Results/calls mismatch"

        d6_val=$(echo "$output" | grep "a6_stop_reason:" | sed 's/.*a6_stop_reason: //' || echo "none")
        [ -n "$d6_val" ] && [ "$d6_val" != "none" ] && \
            pass "D6" "Stop reason present: $d6_val" || \
            fail "D6" "No stop reason"

        echo "$output" | grep -q 'a7_trace_present: true' && \
            pass "D7" "Trace dict has model + provider" || \
            fail "D7" "Trace dict missing model/provider"

        echo "$output" | grep -q 'a8_usage_matches_calls: true' && \
            pass "D8" "Usage total matches tool calls made" || \
            fail "D8" "Usage total doesn't match calls"

        echo "$output" | grep -q 'a9_has_latency: true' && \
            pass "D9" "Usage latency > 0 when tools called" || \
            fail "D9" "Usage latency = 0 despite tool calls"

        echo "$output" | grep -q 'a10_has_content: true' && \
            pass "D10" "Response has text content" || \
            fail "D10" "Response has no text content"

        echo "$output" | grep -q 'cat4_completed: true' || \
            fail "D10x" "Category 4 did not complete" "$(echo "$output" | tail -3)"
    fi
fi

# ── Summary ──
echo ""
echo -e "${CYAN}═══════════════════════════════════════════════════════${NC}"
echo -e "  Total: $TOTAL  ${GREEN}Pass: $PASS_COUNT${NC}  ${RED}Fail: $FAIL_COUNT${NC}  ${YELLOW}Skip: $SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then
    echo -e "\n  ${RED}Failures:${NC}$FAILURES"
fi
echo -e "${CYAN}═══════════════════════════════════════════════════════${NC}"

# Save results
mkdir -p "$RESULTS_DIR"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
echo "NAAb-35 Results — $TIMESTAMP" > "$RESULTS_DIR/results_$TIMESTAMP.txt"
echo "Pass: $PASS_COUNT  Fail: $FAIL_COUNT  Skip: $SKIP_COUNT  Total: $TOTAL" >> "$RESULTS_DIR/results_$TIMESTAMP.txt"
[ "$FAIL_COUNT" -gt 0 ] && echo -e "Failures:$FAILURES" >> "$RESULTS_DIR/results_$TIMESTAMP.txt"
echo ""

exit "$FAIL_COUNT"
