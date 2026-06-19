#!/usr/bin/env bash
# ============================================================================
# NAAb-40: Post-v1.8.0 Regression — Gorilla Test (Rewrite)
# 60 assertions across 6 categories, all offline (no API keys needed):
#   Cat 1: Scoring Calibration — Behavioral Effect (10)
#   Cat 2: Stdlib & VM Parity (10)
#   Cat 3: Dashboard Deadlock Fix + Scoring Output (10)
#   Cat 4: DETECT-Level Violations + HARD Error (10)
#   Cat 5: Python Import Hook Coverage (10)
#   Cat 6: Unicode Normalization + Stdlib Module Sync (10)
# Usage: bash run-naab40.sh [--cat N]
# ============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../../build/naab-lang"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="$_SYSTMP/naab40-$$"
RESULTS_DIR="$SCRIPT_DIR/results"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
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

# --- Banned patterns (must never leak in output) ---
BANNED_PATTERNS=(
    "--no-governance"
    "--governance-override"
    "--drift-baseline-save"
    "--sign-governance"
    "--sign-baseline"
    "--keygen"
    "NAAB_SIGNING_KEY"
    "NAAB_GOVERN_KEY"
    "soft-mandatory"
    "soft.mandatory"
)

check_banned() {
    local output="$1"
    for pat in "${BANNED_PATTERNS[@]}"; do
        if echo "$output" | grep -qi -- "$pat"; then
            echo "$pat"
            return 0
        fi
    done
    return 1
}

# --- Trust store setup ---
source "$SCRIPT_DIR/../../helpers/trust_setup.sh"
setup_isolated_trust
trap 'teardown_isolated_trust; rm -rf "$TEST_TMP"' EXIT

mkdir -p "$TEST_TMP"
"$NAAB" --keygen "$TEST_TMP/test-key.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/test-key.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/test-key.pem"

setup_workdir() {
    local phase="$1"
    local workdir="$TEST_TMP/work-${phase}-$$-$RANDOM"
    mkdir -p "$workdir"
    cp "$SCRIPT_DIR/phases/${phase}.json" "$workdir/govern.json"
    if [ -f "$NAAB_SIGNING_KEY" ]; then
        (cd "$workdir" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
    fi
    echo "$workdir"
}

run_in() {
    local workdir="$1" naab_file="$2"
    cp "$SCRIPT_DIR/src/$naab_file" "$workdir/"
    (cd "$workdir" && timeout 120 "$NAAB" "$naab_file" 2>/dev/null)
}

# Run with separated stdout/stderr
run_in_split() {
    local workdir="$1" naab_file="$2" stderr_file="$3"
    cp "$SCRIPT_DIR/src/$naab_file" "$workdir/"
    (cd "$workdir" && timeout 120 "$NAAB" --governance-dashboard "$naab_file" 2>"$stderr_file")
}

# Run subprocess test, capture exit code
run_subprocess() {
    local workdir="$1" naab_file="$2"
    cp "$SCRIPT_DIR/src/$naab_file" "$workdir/"
    local out exit_code
    out=$(cd "$workdir" && timeout 30 "$NAAB" "$naab_file" 2>&1) && exit_code=0 || exit_code=$?
    echo "$out"
    return $exit_code
}

# --- Header ---
echo -e "${BOLD}${CYAN}+----------------------------------------------------------+${NC}"
echo -e "${BOLD}${CYAN}|  NAAb-40: Post-v1.8.0 Regression — Gorilla Test          |${NC}"
echo -e "${BOLD}${CYAN}|  60 assertions across 6 categories (all offline)          |${NC}"
echo -e "${BOLD}${CYAN}+----------------------------------------------------------+${NC}"
echo ""

# ======================================================================
# CATEGORY 1: Scoring Calibration — Behavioral Effect (10)
# ======================================================================
if should_run 1; then
    echo -e "${CYAN}══════════════════════════════════════════════════════${NC}"
    echo -e "${CYAN}  Category 1: Scoring Calibration — Behavioral Effect (10)${NC}"
    echo -e "${CYAN}══════════════════════════════════════════════════════${NC}"
    echo ""

    WORKDIR=$(setup_workdir "cat1-calibration")
    output=$(run_in "$WORKDIR" "cat1_calibration.naab")
    cat1_exit=$?

    if leaked=$(check_banned "$output"); then
        fail "BAN-1" "Output leaks bypass hint" "$leaked"
    fi

    # A1: calibrate() returns {success: true}
    echo "$output" | grep -q 'a1_calibrate_success: true' && \
        pass "A1" "calibrate() returns {success: true}" || \
        fail "A1" "calibrate() didn't return success" "$(echo "$output" | grep 'a1_' | head -1)"

    # A2: calibration() nested dict has weight matching input
    echo "$output" | grep -q 'a2_weight_match: true' && \
        pass "A2" "calibration() weight matches input" || \
        fail "A2" "calibration() weight mismatch" "$(echo "$output" | grep 'a2_' | head -1)"

    # A3: observation_count increments
    echo "$output" | grep -q 'a3_observation_count: true' && \
        pass "A3" "observation_count increments on repeat" || \
        fail "A3" "observation_count didn't increment" "$(echo "$output" | grep 'a3_' | head -1)"

    # A4: overwrite — latest weight wins
    echo "$output" | grep -q 'a4_overwrite_latest: true' && \
        pass "A4" "overwrite same rule — latest weight wins" || \
        fail "A4" "overwrite didn't take latest" "$(echo "$output" | grep 'a4_' | head -1)"

    # A5: negative weight clamped to 0
    echo "$output" | grep -q 'a5_negative_clamped: true' && \
        pass "A5" "negative weight clamped to 0" || \
        fail "A5" "negative weight not clamped" "$(echo "$output" | grep 'a5_' | head -1)"

    # A6: multiple rules accumulate independently
    echo "$output" | grep -q 'a6_multi_rules: true' && \
        pass "A6" "multiple rules accumulate independently" || \
        fail "A6" "multiple rules failed" "$(echo "$output" | grep 'a6_' | head -1)"

    # A7: bad arity throws
    echo "$output" | grep -q 'a7_bad_arity: true' && \
        pass "A7" "bad args (2 args) throws" || \
        fail "A7" "bad arity didn't throw" "$(echo "$output" | grep 'a7_' | head -1)"

    # A8: bad type throws
    echo "$output" | grep -q 'a8_bad_type: true' && \
        pass "A8" "bad type (string weight) throws" || \
        fail "A8" "bad type didn't throw" "$(echo "$output" | grep 'a8_' | head -1)"

    # A9: reason string matches input
    echo "$output" | grep -q 'a9_reason_match: true' && \
        pass "A9" "calibration() reason matches input" || \
        fail "A9" "reason mismatch" "$(echo "$output" | grep 'a9_' | head -1)"

    # A10: updated_at is non-empty string
    echo "$output" | grep -q 'a10_updated_at: true' && \
        pass "A10" "calibration() updated_at non-empty" || \
        fail "A10" "updated_at empty/missing" "$(echo "$output" | grep 'a10_' | head -1)"

    echo ""
fi

# ======================================================================
# CATEGORY 2: Stdlib & VM Parity (10)
# ======================================================================
if should_run 2; then
    echo -e "${CYAN}══════════════════════════════════════════════════════${NC}"
    echo -e "${CYAN}  Category 2: Stdlib & VM Parity (10)${NC}"
    echo -e "${CYAN}══════════════════════════════════════════════════════${NC}"
    echo ""

    WORKDIR=$(setup_workdir "cat2-stdlib")
    output=$(run_in "$WORKDIR" "cat2_stdlib_parity.naab")
    cat2_exit=$?

    if leaked=$(check_banned "$output"); then
        fail "BAN-2" "Output leaks bypass hint" "$leaked"
    fi

    echo "$output" | grep -q 'b1_sorted_nonmutating: true' && \
        pass "B1" "array.sorted() non-mutating" || \
        fail "B1" "sorted() mutated original" "$(echo "$output" | grep 'b1_' | head -1)"

    echo "$output" | grep -q 'b2_sort_mutating: true' && \
        pass "B2" "array.sort() mutates in place" || \
        fail "B2" "sort() didn't mutate" "$(echo "$output" | grep 'b2_' | head -1)"

    echo "$output" | grep -q 'b3_sorted_comparator: true' && \
        pass "B3" "array.sorted() with comparator" || \
        fail "B3" "sorted() comparator failed" "$(echo "$output" | grep 'b3_' | head -1)"

    echo "$output" | grep -q 'b4_struct_get: true' && \
        pass "B4" "struct .get() works" || \
        fail "B4" "struct .get() failed" "$(echo "$output" | grep 'b4_' | head -1)"

    echo "$output" | grep -q 'b5_struct_get_missing: true' && \
        pass "B5" "struct .get() returns null for missing" || \
        fail "B5" "struct .get() missing key failed" "$(echo "$output" | grep 'b5_' | head -1)"

    echo "$output" | grep -q 'b6_struct_has: true' && \
        pass "B6" "struct .has() works" || \
        fail "B6" "struct .has() failed" "$(echo "$output" | grep 'b6_' | head -1)"

    echo "$output" | grep -q 'b7_struct_keys: true' && \
        pass "B7" "struct .keys() returns array" || \
        fail "B7" "struct .keys() failed" "$(echo "$output" | grep 'b7_' | head -1)"

    echo "$output" | grep -q 'b8_struct_values: true' && \
        pass "B8" "struct .values() returns array" || \
        fail "B8" "struct .values() failed" "$(echo "$output" | grep 'b8_' | head -1)"

    echo "$output" | grep -q 'b9_struct_size: true' && \
        pass "B9" "struct .size() returns count" || \
        fail "B9" "struct .size() failed" "$(echo "$output" | grep 'b9_' | head -1)"

    echo "$output" | grep -q 'b10_string_ops: true' && \
        pass "B10" "string operations (upper/lower/trim/contains)" || \
        fail "B10" "string ops failed" "$(echo "$output" | grep 'b10_' | head -1)"

    echo ""
fi

# ======================================================================
# CATEGORY 3: Dashboard Deadlock Fix + Scoring Output (10)
# ======================================================================
if should_run 3; then
    echo -e "${CYAN}══════════════════════════════════════════════════════${NC}"
    echo -e "${CYAN}  Category 3: Dashboard Deadlock Fix + Scoring Output (10)${NC}"
    echo -e "${CYAN}══════════════════════════════════════════════════════${NC}"
    echo ""

    WORKDIR_DASH=$(setup_workdir "cat3-dashboard")
    STDERR_FILE="$TEST_TMP/cat3_stderr.txt"

    # C1-C6: Main dashboard test with violations
    dash_stdout=$(run_in_split "$WORKDIR_DASH" "cat3_dashboard_violations.naab" "$STDERR_FILE") && dash_exit=0 || dash_exit=$?

    # C1: Dashboard completes (no deadlock — timeout would give 124)
    if [ "$dash_exit" -ne 124 ]; then
        pass "C1" "dashboard completes (no deadlock)"
    else
        fail "C1" "dashboard timed out (deadlock?)" "exit=$dash_exit"
    fi

    # C2: Dashboard header present
    if grep -q "Governance Summary" "$STDERR_FILE" 2>/dev/null; then
        pass "C2" "dashboard header present"
    else
        fail "C2" "dashboard header missing" "$(head -5 "$STDERR_FILE" 2>/dev/null)"
    fi

    # C3: "Checks:" line with count
    if grep -qE "Checks:.*[0-9]+ passed" "$STDERR_FILE" 2>/dev/null; then
        pass "C3" "Checks line with count"
    else
        fail "C3" "Checks line missing" "$(grep -i 'check' "$STDERR_FILE" 2>/dev/null | head -1)"
    fi

    # C4: "Risk score:" line when scoring+violations
    if grep -qE "[Rr]isk [Ss]core:? *[0-9]+" "$STDERR_FILE" 2>/dev/null; then
        pass "C4" "Risk score line present"
    else
        fail "C4" "Risk score line missing" "$(grep -i 'score\|risk' "$STDERR_FILE" 2>/dev/null | head -1)"
    fi

    # C5: Score breakdown shows rule with weight
    if grep -qE "^\s*\+[0-9]+" "$STDERR_FILE" 2>/dev/null; then
        pass "C5" "score breakdown shows +N weight"
    else
        fail "C5" "score breakdown missing" "$(grep -E '\+[0-9]' "$STDERR_FILE" 2>/dev/null | head -1)"
    fi

    # C6: Mode shows "enforce"
    if grep -qi "enforce" "$STDERR_FILE" 2>/dev/null; then
        pass "C6" "mode shows enforce"
    else
        fail "C6" "mode doesn't show enforce" "$(grep -i 'mode' "$STDERR_FILE" 2>/dev/null | head -1)"
    fi

    # C7: weight=0 calibration suppresses rule from breakdown
    # The dashboard_violations.naab calibrates DASH-TRIGGER to weight=0 before triggers fire
    # Check that DASH-TRIGGER doesn't appear with +N (non-zero) prefix in score breakdown
    if ! grep -E "^\s*\+[1-9][0-9]*.*DASH-TRIGGER" "$STDERR_FILE" 2>/dev/null; then
        pass "C7" "weight=0 suppresses rule from scored breakdown"
    else
        fail "C7" "weight=0 didn't suppress" "$(grep 'DASH-TRIGGER' "$STDERR_FILE" 2>/dev/null | head -1)"
    fi

    # C8: weight=500 calibration shows amplified weight
    WORKDIR_AMP=$(setup_workdir "cat3-dashboard")
    STDERR_AMP="$TEST_TMP/cat3_stderr_amp.txt"
    amp_stdout=$(run_in_split "$WORKDIR_AMP" "cat3_dashboard_amplify.naab" "$STDERR_AMP") && amp_exit=0 || amp_exit=$?

    if grep -qE "\+500" "$STDERR_AMP" 2>/dev/null; then
        pass "C8" "weight=500 shows amplified weight"
    else
        fail "C8" "amplified weight not shown" "$(grep -E '\+[0-9]' "$STDERR_AMP" 2>/dev/null | head -1)"
    fi

    # C9: No integrity mismatch warning
    if ! grep -qi "mismatch" "$STDERR_FILE" 2>/dev/null; then
        pass "C9" "no integrity mismatch warning"
    else
        fail "C9" "integrity mismatch found" "$(grep -i 'mismatch' "$STDERR_FILE" 2>/dev/null | head -1)"
    fi

    # C10: Dashboard completes under load (5+ violations)
    WORKDIR_STRESS=$(setup_workdir "cat3-dashboard")
    STDERR_STRESS="$TEST_TMP/cat3_stderr_stress.txt"
    stress_stdout=$(run_in_split "$WORKDIR_STRESS" "cat3_dashboard_stress.naab" "$STDERR_STRESS") && stress_exit=0 || stress_exit=$?

    if [ "$stress_exit" -ne 124 ]; then
        pass "C10" "dashboard completes under load (5+ violations)"
    else
        fail "C10" "dashboard deadlock under load" "exit=$stress_exit"
    fi

    echo ""
fi

# ======================================================================
# CATEGORY 4: DETECT-Level Violations + HARD Error (10)
# ======================================================================
if should_run 4; then
    echo -e "${CYAN}══════════════════════════════════════════════════════${NC}"
    echo -e "${CYAN}  Category 4: DETECT-Level Violations + HARD Error (10)${NC}"
    echo -e "${CYAN}══════════════════════════════════════════════════════${NC}"
    echo ""

    # D1, D3, D4, D5: DETECT caught by try/catch (inline)
    WORKDIR_D1=$(setup_workdir "cat4-detect")
    d1_output=$(run_in "$WORKDIR_D1" "cat4_detect_catch.naab") && d1_exit=0 || d1_exit=$?

    # D1: DETECT violation catchable
    if echo "$d1_output" | grep -q 'DETECT_CAUGHT: true' && [ "$d1_exit" -eq 0 ]; then
        pass "D1" "DETECT violation catchable by try/catch"
    else
        fail "D1" "DETECT not catchable" "exit=$d1_exit, $(echo "$d1_output" | grep 'DETECT_CAUGHT' | head -1)"
    fi

    # D2: DETECT without try/catch exits non-zero
    WORKDIR_D2=$(setup_workdir "cat4-detect")
    d2_output=$(run_subprocess "$WORKDIR_D2" "cat4_detect_uncaught.naab") && d2_exit=0 || d2_exit=$?

    if [ "$d2_exit" -ne 0 ]; then
        pass "D2" "DETECT without try/catch exits non-zero"
    else
        fail "D2" "DETECT without catch exited 0" "exit=$d2_exit"
    fi

    # D3: Caught DETECT error contains rule info
    if echo "$d1_output" | grep -q 'd3_has_info: true'; then
        pass "D3" "caught DETECT error contains rule info"
    else
        fail "D3" "caught DETECT error lacks rule info" "$(echo "$d1_output" | grep 'd3_' | head -1)"
    fi

    # D4: Execution continues after DETECT catch
    if echo "$d1_output" | grep -q 'CONTINUED'; then
        pass "D4" "execution continues after DETECT catch"
    else
        fail "D4" "execution didn't continue after catch"
    fi

    # D5: Multiple DETECT catches in same program
    if echo "$d1_output" | grep -q 'CAUGHT_1: true' && echo "$d1_output" | grep -q 'CAUGHT_2: true'; then
        pass "D5" "multiple DETECT catches in same program"
    else
        fail "D5" "multiple catches failed" "$(echo "$d1_output" | grep 'CAUGHT_' | head -2)"
    fi

    # D6: HARD block (blocked language ruby) exits 3
    WORKDIR_D6=$(setup_workdir "cat4-detect")
    d6_output=$(run_subprocess "$WORKDIR_D6" "cat4_hard_ruby.naab") && d6_exit=0 || d6_exit=$?

    if [ "$d6_exit" -eq 3 ]; then
        pass "D6" "HARD block (ruby) exits 3"
    else
        fail "D6" "HARD block wrong exit code" "expected=3 got=$d6_exit"
    fi

    # D7: HARD block uncatchable by try/catch
    WORKDIR_D7=$(setup_workdir "cat4-detect")
    d7_output=$(run_subprocess "$WORKDIR_D7" "cat4_hard_trycatch.naab") && d7_exit=0 || d7_exit=$?

    if [ "$d7_exit" -eq 3 ] && ! echo "$d7_output" | grep -q "caught"; then
        pass "D7" "HARD block uncatchable by try/catch"
    else
        fail "D7" "HARD block was catchable" "exit=$d7_exit, $(echo "$d7_output" | grep 'caught' | head -1)"
    fi

    # D8: HARD block output doesn't leak bypass hints
    if leaked=$(check_banned "$d6_output"); then
        fail "D8" "HARD block output leaks bypass hint" "$leaked"
    else
        pass "D8" "HARD block output clean (no bypass hints)"
    fi

    # D9: DETECT doesn't produce exit 3
    if [ "$d2_exit" -ne 3 ]; then
        pass "D9" "DETECT doesn't produce exit 3"
    else
        fail "D9" "DETECT produced exit 3 (should be runtime error)" "exit=$d2_exit"
    fi

    # D10: Clean code with no DETECT pattern → exit 0
    WORKDIR_D10=$(setup_workdir "cat4-detect")
    d10_output=$(run_subprocess "$WORKDIR_D10" "cat4_clean.naab") && d10_exit=0 || d10_exit=$?

    if [ "$d10_exit" -eq 0 ]; then
        pass "D10" "clean code exits 0"
    else
        fail "D10" "clean code exited non-zero" "exit=$d10_exit"
    fi

    echo ""
fi

# ======================================================================
# CATEGORY 5: Python Import Hook Coverage (10)
# ======================================================================
if should_run 5; then
    echo -e "${CYAN}══════════════════════════════════════════════════════${NC}"
    echo -e "${CYAN}  Category 5: Python Import Hook Coverage (10)${NC}"
    echo -e "${CYAN}══════════════════════════════════════════════════════${NC}"
    echo ""

    # E1: import subprocess blocked
    WORKDIR_E1=$(setup_workdir "cat5-imports")
    e1_output=$(run_subprocess "$WORKDIR_E1" "cat5_e1_subprocess.naab") && e1_exit=0 || e1_exit=$?
    if [ "$e1_exit" -eq 3 ]; then
        pass "E1" "import subprocess blocked (exit 3)"
    else
        fail "E1" "import subprocess not blocked" "exit=$e1_exit"
    fi

    # E2: import ctypes blocked
    WORKDIR_E2=$(setup_workdir "cat5-imports")
    e2_output=$(run_subprocess "$WORKDIR_E2" "cat5_e2_ctypes.naab") && e2_exit=0 || e2_exit=$?
    if [ "$e2_exit" -eq 3 ]; then
        pass "E2" "import ctypes blocked (exit 3)"
    else
        fail "E2" "import ctypes not blocked" "exit=$e2_exit"
    fi

    # E3: __import__("os") dynamic import blocked
    # Dynamic imports are caught by the Python import hook at runtime (not static scan),
    # so they exit with a Python error (non-zero), not necessarily exit 3
    WORKDIR_E3=$(setup_workdir "cat5-imports")
    e3_output=$(run_subprocess "$WORKDIR_E3" "cat5_e3_dunder_import.naab") && e3_exit=0 || e3_exit=$?
    if [ "$e3_exit" -ne 0 ] && ! echo "$e3_output" | grep -q "bypass"; then
        pass "E3" "__import__(\"os\") blocked (exit $e3_exit)"
    else
        fail "E3" "__import__(\"os\") not blocked" "exit=$e3_exit"
    fi

    # E4: importlib.import_module("subprocess") blocked
    WORKDIR_E4=$(setup_workdir "cat5-imports")
    e4_output=$(run_subprocess "$WORKDIR_E4" "cat5_e4_importlib.naab") && e4_exit=0 || e4_exit=$?
    if [ "$e4_exit" -ne 0 ] && ! echo "$e4_output" | grep -q "bypass"; then
        pass "E4" "importlib.import_module blocked (exit $e4_exit)"
    else
        fail "E4" "importlib.import_module not blocked" "exit=$e4_exit"
    fi

    # E5: __import__("o"+"s") concat blocked
    WORKDIR_E5=$(setup_workdir "cat5-imports")
    e5_output=$(run_subprocess "$WORKDIR_E5" "cat5_e5_concat.naab") && e5_exit=0 || e5_exit=$?
    if [ "$e5_exit" -ne 0 ] && ! echo "$e5_output" | grep -q "bypass"; then
        pass "E5" "__import__(concat) blocked (exit $e5_exit)"
    else
        fail "E5" "__import__(concat) not blocked" "exit=$e5_exit"
    fi

    # E6: from os import system blocked
    WORKDIR_E6=$(setup_workdir "cat5-imports")
    e6_output=$(run_subprocess "$WORKDIR_E6" "cat5_e6_from_import.naab") && e6_exit=0 || e6_exit=$?
    if [ "$e6_exit" -eq 3 ]; then
        pass "E6" "from os import system blocked (exit 3)"
    else
        fail "E6" "from os import system not blocked" "exit=$e6_exit"
    fi

    # E7: import socket blocked
    WORKDIR_E7=$(setup_workdir "cat5-imports")
    e7_output=$(run_subprocess "$WORKDIR_E7" "cat5_e7_socket.naab") && e7_exit=0 || e7_exit=$?
    if [ "$e7_exit" -eq 3 ]; then
        pass "E7" "import socket blocked (exit 3)"
    else
        fail "E7" "import socket not blocked" "exit=$e7_exit"
    fi

    # E8: import json allowed
    WORKDIR_E8=$(setup_workdir "cat5-imports")
    e8_output=$(run_subprocess "$WORKDIR_E8" "cat5_e8_json_allowed.naab") && e8_exit=0 || e8_exit=$?
    if [ "$e8_exit" -eq 0 ] && echo "$e8_output" | grep -q "ok"; then
        pass "E8" "import json allowed (exit 0)"
    else
        fail "E8" "import json failed" "exit=$e8_exit, $(echo "$e8_output" | head -1)"
    fi

    # E9: import math allowed
    WORKDIR_E9=$(setup_workdir "cat5-imports")
    e9_output=$(run_subprocess "$WORKDIR_E9" "cat5_e9_math_allowed.naab") && e9_exit=0 || e9_exit=$?
    if [ "$e9_exit" -eq 0 ] && echo "$e9_output" | grep -q "4"; then
        pass "E9" "import math allowed (exit 0)"
    else
        fail "E9" "import math failed" "exit=$e9_exit, $(echo "$e9_output" | head -1)"
    fi

    # E10: Blocked import error mentions governance/blocked/policy
    if echo "$e1_output" | grep -qiE "governance|blocked|policy|prohibited|denied"; then
        pass "E10" "blocked import error mentions governance"
    else
        fail "E10" "blocked import error lacks governance mention" "$(echo "$e1_output" | head -3)"
    fi

    echo ""
fi

# ======================================================================
# CATEGORY 6: Unicode Normalization + Stdlib Module Sync (10)
# ======================================================================
if should_run 6; then
    echo -e "${CYAN}══════════════════════════════════════════════════════${NC}"
    echo -e "${CYAN}  Category 6: Unicode Normalization + Stdlib Module Sync (10)${NC}"
    echo -e "${CYAN}══════════════════════════════════════════════════════${NC}"
    echo ""

    # F1-F4: Unicode normalization tests — generate .naab files with UTF-8 bytes
    WORKDIR_UNI=$(setup_workdir "cat6-unicode-stdlib")

    # F1: Subscript ₘ (U+2098) → os.systeₘ("ls") should be caught
    cat > "$WORKDIR_UNI/f1_subscript_m.naab" << 'NAABEOF'
main {
    let r = <<python
NAABEOF
    printf 'os.syste\xe2\x82\x98("ls")\n' >> "$WORKDIR_UNI/f1_subscript_m.naab"
    cat >> "$WORKDIR_UNI/f1_subscript_m.naab" << 'NAABEOF'
print("bypass")
>>
    print(string(r))
}
NAABEOF
    f1_output=$(cd "$WORKDIR_UNI" && timeout 30 "$NAAB" f1_subscript_m.naab 2>&1) && f1_exit=0 || f1_exit=$?
    if [ "$f1_exit" -ne 0 ]; then
        pass "F1" "subscript U+2098 in os.system blocked"
    else
        fail "F1" "subscript bypass not detected" "exit=$f1_exit"
    fi

    # F2: Subscript ₒ (U+2092) → ₒs.system("ls") should be caught
    # Uses subscript 'o' in 'os.system' — normalizeUnicode maps ₒ→o, pattern matches os.system
    cat > "$WORKDIR_UNI/f2_subscript_o.naab" << 'NAABEOF'
main {
    let r = <<python
NAABEOF
    printf '\xe2\x82\x92s.system("ls")\n' >> "$WORKDIR_UNI/f2_subscript_o.naab"
    cat >> "$WORKDIR_UNI/f2_subscript_o.naab" << 'NAABEOF'
print("bypass")
>>
    print(string(r))
}
NAABEOF
    f2_output=$(cd "$WORKDIR_UNI" && timeout 30 "$NAAB" f2_subscript_o.naab 2>&1) && f2_exit=0 || f2_exit=$?
    if [ "$f2_exit" -ne 0 ]; then
        pass "F2" "subscript U+2092 in os.system blocked"
    else
        fail "F2" "subscript os bypass not detected" "exit=$f2_exit"
    fi

    # F3: Fullwidth ｏ (U+FF4F) → ｏs.system("ls") should be caught
    cat > "$WORKDIR_UNI/f3_fullwidth_o.naab" << 'NAABEOF'
main {
    let r = <<python
NAABEOF
    printf '\xef\xbd\x8fs.system("ls")\n' >> "$WORKDIR_UNI/f3_fullwidth_o.naab"
    cat >> "$WORKDIR_UNI/f3_fullwidth_o.naab" << 'NAABEOF'
print("bypass")
>>
    print(string(r))
}
NAABEOF
    f3_output=$(cd "$WORKDIR_UNI" && timeout 30 "$NAAB" f3_fullwidth_o.naab 2>&1) && f3_exit=0 || f3_exit=$?
    if [ "$f3_exit" -ne 0 ]; then
        pass "F3" "fullwidth U+FF4F in os.system blocked"
    else
        fail "F3" "fullwidth bypass not detected" "exit=$f3_exit"
    fi

    # F4: Plain ASCII os.system still blocked (control)
    cat > "$WORKDIR_UNI/f4_ascii_control.naab" << 'NAABEOF'
main {
    let r = <<python
os.system("ls")
print("bypass")
>>
    print(string(r))
}
NAABEOF
    f4_output=$(cd "$WORKDIR_UNI" && timeout 30 "$NAAB" f4_ascii_control.naab 2>&1) && f4_exit=0 || f4_exit=$?
    if [ "$f4_exit" -ne 0 ]; then
        pass "F4" "ASCII os.system still blocked (control)"
    else
        fail "F4" "ASCII os.system not blocked" "exit=$f4_exit"
    fi

    # F5: use agent loads in VM
    WORKDIR_F5=$(setup_workdir "cat6-unicode-stdlib")
    f5_output=$(run_subprocess "$WORKDIR_F5" "cat6_f5_agent.naab") && f5_exit=0 || f5_exit=$?
    if [ "$f5_exit" -eq 0 ] && echo "$f5_output" | grep -q "agent_ok"; then
        pass "F5" "use agent loads in VM"
    else
        fail "F5" "use agent failed" "exit=$f5_exit, $(echo "$f5_output" | head -1)"
    fi

    # F6: use codegen loads in VM
    WORKDIR_F6=$(setup_workdir "cat6-unicode-stdlib")
    f6_output=$(run_subprocess "$WORKDIR_F6" "cat6_f6_codegen.naab") && f6_exit=0 || f6_exit=$?
    if [ "$f6_exit" -eq 0 ] && echo "$f6_output" | grep -q "codegen_ok"; then
        pass "F6" "use codegen loads in VM"
    else
        fail "F6" "use codegen failed" "exit=$f6_exit, $(echo "$f6_output" | head -1)"
    fi

    # F7: use governance loads + works
    WORKDIR_F7=$(setup_workdir "cat6-unicode-stdlib")
    f7_output=$(run_subprocess "$WORKDIR_F7" "cat6_f7_governance.naab") && f7_exit=0 || f7_exit=$?
    if [ "$f7_exit" -eq 0 ] && echo "$f7_output" | grep -q "healthy"; then
        pass "F7" "use governance loads + works"
    else
        fail "F7" "use governance failed" "exit=$f7_exit, $(echo "$f7_output" | head -1)"
    fi

    # F8: use orchestra loads + works
    WORKDIR_F8=$(setup_workdir "cat6-unicode-stdlib")
    f8_output=$(run_subprocess "$WORKDIR_F8" "cat6_f8_orchestra.naab") && f8_exit=0 || f8_exit=$?
    if [ "$f8_exit" -eq 0 ] && echo "$f8_output" | grep -qi "approved"; then
        pass "F8" "use orchestra loads + works"
    else
        fail "F8" "use orchestra failed" "exit=$f8_exit, $(echo "$f8_output" | head -1)"
    fi

    # F9: All 25 modules importable
    WORKDIR_F9=$(setup_workdir "cat6-unicode-stdlib")
    f9_output=$(run_subprocess "$WORKDIR_F9" "cat6_f9_all25.naab") && f9_exit=0 || f9_exit=$?
    if [ "$f9_exit" -eq 0 ] && echo "$f9_output" | grep -q "all_25: true"; then
        pass "F9" "all 25 modules importable"
    else
        fail "F9" "not all 25 modules loaded" "exit=$f9_exit, $(echo "$f9_output" | head -3)"
    fi

    # F10: Previously-missing modules work in --tree-walk
    WORKDIR_F10=$(setup_workdir "cat6-unicode-stdlib")
    cp "$SCRIPT_DIR/src/cat6_f10_treewalk.naab" "$WORKDIR_F10/"
    f10_output=$(cd "$WORKDIR_F10" && timeout 30 "$NAAB" --tree-walk cat6_f10_treewalk.naab 2>/dev/null) && f10_exit=0 || f10_exit=$?
    if [ "$f10_exit" -eq 0 ] && echo "$f10_output" | grep -q "tw_agent_ok"; then
        pass "F10" "previously-missing modules work in --tree-walk"
    else
        fail "F10" "tree-walk module test failed" "exit=$f10_exit, $(echo "$f10_output" | head -3)"
    fi

    echo ""
fi

# ======================================================================
# Summary
# ======================================================================
echo -e "${BOLD}${CYAN}+----------------------------------------------------------+${NC}"
echo -e "${BOLD}  Results: ${GREEN}${PASS_COUNT} passed${NC}, ${RED}${FAIL_COUNT} failed${NC}, ${YELLOW}${SKIP_COUNT} skipped${NC} / ${TOTAL} total"
echo -e "${BOLD}${CYAN}+----------------------------------------------------------+${NC}"

if [ "$FAIL_COUNT" -gt 0 ]; then
    echo -e "\n${RED}${BOLD}Failures:${NC}"
    echo -e "$FAILURES"
    echo ""
fi

# Save results
mkdir -p "$RESULTS_DIR"
echo "{\"pass\": $PASS_COUNT, \"fail\": $FAIL_COUNT, \"skip\": $SKIP_COUNT, \"total\": $TOTAL}" > "$RESULTS_DIR/summary.json"

exit "$FAIL_COUNT"
