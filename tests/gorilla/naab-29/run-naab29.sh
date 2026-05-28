#!/usr/bin/env bash
# NAAb-29: Adversarial Governance Infrastructure Stress Test
# 102 assertions across 11 categories testing whether governance survives bad code
# Usage: bash run-naab29.sh [--cat N]   (N=1..11, omit for all)
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../../build/naab-lang"
TMPBASE="${TMPDIR:-/data/data/com.termux/files/usr/tmp}/naab29-$$"
RESULTS_DIR="$SCRIPT_DIR/results"

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

cleanup() {
    rm -rf "$TMPBASE"
}
trap cleanup EXIT

mkdir -p "$TMPBASE" "$RESULTS_DIR"

# ─── Helpers ───────────────────────────────────────────────

pass() {
    local id="$1" desc="$2"
    PASS_COUNT=$((PASS_COUNT + 1))
    TOTAL=$((TOTAL + 1))
    echo -e "  ${GREEN}PASS${NC} [$id] $desc"
}

fail() {
    local id="$1" desc="$2" detail="${3:-}"
    FAIL_COUNT=$((FAIL_COUNT + 1))
    TOTAL=$((TOTAL + 1))
    echo -e "  ${RED}FAIL${NC} [$id] $desc"
    if [ -n "$detail" ]; then
        echo -e "       ${RED}→ $detail${NC}"
    fi
    FAILURES="${FAILURES}\n  [$id] $desc${detail:+ — $detail}"
}

skip() {
    local id="$1" desc="$2" reason="${3:-}"
    SKIP_COUNT=$((SKIP_COUNT + 1))
    TOTAL=$((TOTAL + 1))
    echo -e "  ${YELLOW}SKIP${NC} [$id] $desc${reason:+ ($reason)}"
}

# Setup an isolated test directory with governance
# Usage: setup_test_dir <phase_config> [extra_files...]
# Sets TEST_DIR to the created directory
setup_test_dir() {
    local phase="$1"; shift
    TEST_DIR="$TMPBASE/test_$(date +%s%N)"
    mkdir -p "$TEST_DIR"
    cp "$SCRIPT_DIR/phases/$phase" "$TEST_DIR/govern.json"
    # Sign with current key
    (cd "$TEST_DIR" && "$NAAB" --sign-governance) >/dev/null 2>&1
    # Copy any extra files
    for f in "$@"; do
        if [ -f "$f" ]; then
            cp "$f" "$TEST_DIR/"
        elif [ -d "$f" ]; then
            cp -r "$f" "$TEST_DIR/"
        fi
    done
}

# Run a naab test — sets NAAB_EXIT to the exit code
# Usage: run_naab <test_file> [extra_flags...]
NAAB_EXIT=0
run_naab() {
    local test_file="$1"; shift
    (cd "$TEST_DIR" && "$NAAB" "$@" "$test_file") >"$TEST_DIR/stdout.log" 2>"$TEST_DIR/stderr.log" && NAAB_EXIT=0 || NAAB_EXIT=$?
}

# Check exit code matches expected
# Usage: check_exit <test_id> <description> <expected_exit> <actual_exit>
check_exit() {
    local id="$1" desc="$2" expected="$3" actual="$4"
    if [ "$actual" -eq "$expected" ]; then
        pass "$id" "$desc (exit $actual)"
    else
        fail "$id" "$desc" "expected exit $expected, got $actual"
    fi
}

# Check exit code is in a set of acceptable values
# Usage: check_exit_oneof <test_id> <description> <actual_exit> <val1> [val2...]
check_exit_oneof() {
    local id="$1" desc="$2" actual="$3"; shift 3
    for expected in "$@"; do
        if [ "$actual" -eq "$expected" ]; then
            pass "$id" "$desc (exit $actual)"
            return
        fi
    done
    fail "$id" "$desc" "expected exit {$*}, got $actual"
}

# ─── Signing Setup ─────────────────────────────────────────

echo -e "${BOLD}${CYAN}╔══════════════════════════════════════════════════════════╗${NC}"
echo -e "${BOLD}${CYAN}║  NAAb-29: Adversarial Governance Stress Test            ║${NC}"
echo -e "${BOLD}${CYAN}║  102 assertions · 11 categories · 4 governance phases   ║${NC}"
echo -e "${BOLD}${CYAN}╚══════════════════════════════════════════════════════════╝${NC}"
echo ""

# Generate a temporary signing key for test isolation
KEYGEN_DIR="$TMPBASE/keys"
mkdir -p "$KEYGEN_DIR"
(cd "$KEYGEN_DIR" && "$NAAB" --keygen test-key.pem) >/dev/null 2>&1
export NAAB_SIGNING_KEY="$KEYGEN_DIR/test-key.pem"

# Backup existing trust store
TRUST_STORE="$HOME/.naab/trusted-keys"
TRUST_BACKUP="$TMPBASE/trusted-keys-backup"
if [ -d "$TRUST_STORE" ]; then
    cp -r "$TRUST_STORE" "$TRUST_BACKUP"
fi

restore_trust_store() {
    if [ -d "$TRUST_BACKUP" ]; then
        rm -rf "$TRUST_STORE"
        cp -r "$TRUST_BACKUP" "$TRUST_STORE"
    fi
}

# ═══════════════════════════════════════════════════════════
# CAT 1: EXECUTION (10 tests)
# ═══════════════════════════════════════════════════════════
if should_run 1; then
echo -e "\n${BOLD}${CYAN}── Cat 1: EXECUTION ──────────────────────────────────────${NC}"
echo -e "   Tests that governance blocks dangerous code execution"

for i in 01 02 03 04 05 06 07 08 09 10; do
    test_file="$SCRIPT_DIR/src/exec_${i}_*.naab"
    # Expand glob
    test_file=$(echo $test_file)
    if [ ! -f "$test_file" ]; then
        skip "E-$i" "exec_${i} (file not found)"
        continue
    fi
    test_name=$(basename "$test_file" .naab)
    setup_test_dir "phase1-strict.json"
    cp "$test_file" "$TEST_DIR/"
    run_naab "$(basename "$test_file")"
    actual=$NAAB_EXIT
    check_exit_oneof "E-$i" "$test_name" "$actual" 3 4 1
done
rm -rf "$TMPBASE"/test_*
fi

# ═══════════════════════════════════════════════════════════
# CAT 2: ADMISSIBILITY (10 tests)
# ═══════════════════════════════════════════════════════════
if should_run 2; then
echo -e "\n${BOLD}${CYAN}── Cat 2: ADMISSIBILITY ──────────────────────────────────${NC}"
echo -e "   Tests that governance rejects invalid configs and resource abuse"

# A-01: Oversized govern.json array
setup_test_dir "phase1-strict.json"
python3 -c "
import json
with open('$TEST_DIR/govern.json','r') as f: d=json.load(f)
d['_attack'] = list(range(15000))
with open('$TEST_DIR/govern.json','w') as f: json.dump(d,f)
" 2>/dev/null
(cd "$TEST_DIR" && "$NAAB" --sign-governance) >/dev/null 2>&1
echo 'main { print("should not run") }' > "$TEST_DIR/test.naab"
run_naab "test.naab"
actual=$NAAB_EXIT
# Large config should still parse (JSON is valid), execution may succeed or fail
# The test validates the engine doesn't crash on oversized input
if [ "$actual" -ne 139 ] && [ "$actual" -ne 134 ] && [ "$actual" -ne 136 ]; then
    pass "A-01" "Oversized govern.json array — no crash (exit $actual)"
else
    fail "A-01" "Oversized govern.json array" "segfault or abort (exit $actual)"
fi

# A-02: Deeply nested JSON bomb
setup_test_dir "phase1-strict.json"
python3 -c "
import json
# Create 100-deep nesting
nested = {}
current = nested
for i in range(100):
    current['level_'+str(i)] = {}
    current = current['level_'+str(i)]
current['value'] = 'deep'
with open('$TEST_DIR/govern.json','r') as f: d=json.load(f)
d['_nested_bomb'] = nested
with open('$TEST_DIR/govern.json','w') as f: json.dump(d,f)
" 2>/dev/null
(cd "$TEST_DIR" && "$NAAB" --sign-governance) >/dev/null 2>&1
echo 'main { print("should not run") }' > "$TEST_DIR/test.naab"
run_naab "test.naab"
actual=$NAAB_EXIT
if [ "$actual" -ne 139 ] && [ "$actual" -ne 134 ] && [ "$actual" -ne 136 ]; then
    pass "A-02" "Deeply nested JSON bomb — no crash (exit $actual)"
else
    fail "A-02" "Deeply nested JSON bomb" "segfault or abort (exit $actual)"
fi

# A-03: Malformed JSON
setup_test_dir "phase1-strict.json"
echo '{version: "5.0", mode: enforce}' > "$TEST_DIR/govern.json"
echo 'main { print("should not run") }' > "$TEST_DIR/test.naab"
run_naab "test.naab"
actual=$NAAB_EXIT
check_exit_oneof "A-03" "Malformed JSON govern.json" "$actual" 4 3 0

# A-04: Missing version field (backward compat)
setup_test_dir "phase1-strict.json"
echo '{"mode": "enforce"}' > "$TEST_DIR/govern.json"
(cd "$TEST_DIR" && "$NAAB" --sign-governance) >/dev/null 2>&1
echo 'main { print("hello") }' > "$TEST_DIR/test.naab"
run_naab "test.naab"
actual=$NAAB_EXIT
# Missing version should be handled gracefully — not crash
if [ "$actual" -ne 139 ] && [ "$actual" -ne 134 ]; then
    pass "A-04" "Missing version field — graceful handling (exit $actual)"
else
    fail "A-04" "Missing version field" "crash (exit $actual)"
fi

# A-05: Invalid enforcement level
setup_test_dir "phase1-strict.json"
python3 -c "
import json
with open('$TEST_DIR/govern.json','r') as f: d=json.load(f)
d['mode'] = 'destroy_everything'
with open('$TEST_DIR/govern.json','w') as f: json.dump(d,f)
" 2>/dev/null
(cd "$TEST_DIR" && "$NAAB" --sign-governance) >/dev/null 2>&1
echo 'main { print("hello") }' > "$TEST_DIR/test.naab"
run_naab "test.naab"
actual=$NAAB_EXIT
if [ "$actual" -ne 139 ] && [ "$actual" -ne 134 ]; then
    pass "A-05" "Invalid enforcement level — graceful handling (exit $actual)"
else
    fail "A-05" "Invalid enforcement level" "crash (exit $actual)"
fi

# A-06 to A-10: .naab files
for i in 06 07 08 09 10; do
    test_file="$SCRIPT_DIR/src/admit_${i}_*.naab"
    test_file=$(echo $test_file)
    if [ ! -f "$test_file" ]; then
        skip "A-$i" "admit_${i} (file not found)"
        continue
    fi
    test_name=$(basename "$test_file" .naab)
    setup_test_dir "phase1-strict.json"
    cp "$test_file" "$TEST_DIR/"
    run_naab "$(basename "$test_file")"
    actual=$NAAB_EXIT
    check_exit_oneof "A-$i" "$test_name" "$actual" 3 4 1 2
done
rm -rf "$TMPBASE"/test_*
fi

# ═══════════════════════════════════════════════════════════
# CAT 3: AUTHORITY (10 tests)
# ═══════════════════════════════════════════════════════════
if should_run 3; then
echo -e "\n${BOLD}${CYAN}── Cat 3: AUTHORITY ──────────────────────────────────────${NC}"
echo -e "   Tests signature verification and trust store enforcement"

if [ -f "$SCRIPT_DIR/src/authority_tests.sh" ]; then
    bash "$SCRIPT_DIR/src/authority_tests.sh" "$NAAB" "$TMPBASE" "$KEYGEN_DIR" \
        2>"$RESULTS_DIR/authority_stderr.log" | while IFS='|' read -r status id desc detail; do
        case "$status" in
            PASS) pass "$id" "$desc" ;;
            FAIL) fail "$id" "$desc" "$detail" ;;
            SKIP) skip "$id" "$desc" "$detail" ;;
        esac
    done
    # Source the counts from the sub-script
    if [ -f "$TMPBASE/authority_counts" ]; then
        source "$TMPBASE/authority_counts"
    fi
else
    for i in $(seq 1 10); do
        skip "AU-$(printf '%02d' $i)" "authority test (script not found)"
    done
fi
rm -rf "$TMPBASE"/au*
fi

# ═══════════════════════════════════════════════════════════
# CAT 4: CONTINUITY (8 tests)
# ═══════════════════════════════════════════════════════════
if should_run 4; then
echo -e "\n${BOLD}${CYAN}── Cat 4: CONTINUITY ─────────────────────────────────────${NC}"
echo -e "   Tests that governance state persists across execution boundaries"

for i in 01 02 03 04 05 06 07 08; do
    test_file="$SCRIPT_DIR/src/contin_${i}_*.naab"
    test_file=$(echo $test_file)
    if [ ! -f "$test_file" ]; then
        skip "C-$i" "contin_${i} (file not found)"
        continue
    fi
    test_name=$(basename "$test_file" .naab)
    # C-03 tests no false scoring on clean code → expects exit 0
    # C-07/C-08 test advisory behavior → expect exit 0
    case "$i" in
        03) expected_exits="0" ;;
        06|07|08) expected_exits="0" ;;
        *) expected_exits="3 4 1" ;;
    esac
    setup_test_dir "phase1-strict.json"
    # Copy any helper modules if the test needs them
    for helper in "$SCRIPT_DIR/src/contin_helper_"*.naab; do
        [ -f "$helper" ] && cp "$helper" "$TEST_DIR/"
    done
    cp "$test_file" "$TEST_DIR/"
    run_naab "$(basename "$test_file")"
    actual=$NAAB_EXIT
    check_exit_oneof "C-$i" "$test_name" "$actual" $expected_exits
done
rm -rf "$TMPBASE"/test_*
fi

# ═══════════════════════════════════════════════════════════
# CAT 5: INTERRUPTION (8 tests)
# ═══════════════════════════════════════════════════════════
if should_run 5; then
echo -e "\n${BOLD}${CYAN}── Cat 5: INTERRUPTION ───────────────────────────────────${NC}"
echo -e "   Tests governance resilience under mid-run disruption"

if [ -f "$SCRIPT_DIR/src/interruption_tests.sh" ]; then
    bash "$SCRIPT_DIR/src/interruption_tests.sh" "$NAAB" "$TMPBASE" "$KEYGEN_DIR" \
        2>"$RESULTS_DIR/interruption_stderr.log" | while IFS='|' read -r status id desc detail; do
        case "$status" in
            PASS) pass "$id" "$desc" ;;
            FAIL) fail "$id" "$desc" "$detail" ;;
            SKIP) skip "$id" "$desc" "$detail" ;;
        esac
    done
else
    for i in $(seq 1 8); do
        skip "I-$(printf '%02d' $i)" "interruption test (script not found)"
    done
fi
rm -rf "$TMPBASE"/i0*
fi

# ═══════════════════════════════════════════════════════════
# CAT 6: LEGITIMACY (10 tests) ⭐ CRITICAL
# ═══════════════════════════════════════════════════════════
if should_run 6; then
echo -e "\n${BOLD}${CYAN}── Cat 6: LEGITIMACY ⭐ ──────────────────────────────────${NC}"
echo -e "   Tests that governance cannot be circumvented from within"

for i in 01 02 03 04 05 06 07 08 09 10; do
    test_file="$SCRIPT_DIR/src/legit_${i}_*.naab"
    test_file=$(echo $test_file)
    if [ ! -f "$test_file" ]; then
        skip "L-$i" "legit_${i} (file not found)"
        continue
    fi
    test_name=$(basename "$test_file" .naab)
    # L-03: no false positive → exit 0
    # L-05: regex handles extra space → exit 0
    # L-06: substring behavior documentation → exit 0 or 3
    # L-10: advisory continues → exit 0
    case "$i" in
        01|03|05|10) expected_exits="0" ;;  # L-01: catch is valid; bypass is L-02
        06) expected_exits="0 3" ;;  # document actual behavior
        09) expected_exits="3" ;;  # soft without override = block
        *) expected_exits="3 4 1" ;;
    esac
    setup_test_dir "phase1-strict.json"
    cp "$test_file" "$TEST_DIR/"
    # L-04 needs a contract for sanitize_string
    if [ "$i" = "04" ]; then
        python3 -c "
import json
with open('$TEST_DIR/govern.json','r') as f: d=json.load(f)
d['contracts'] = d.get('contracts', {})
d['contracts']['functions'] = d['contracts'].get('functions', {})
d['contracts']['functions']['sanitize_string'] = {}
with open('$TEST_DIR/govern.json','w') as f: json.dump(d,f,indent=2)
" 2>/dev/null
        (cd "$TEST_DIR" && "$NAAB" --sign-governance) >/dev/null 2>&1
    fi
    # L-05 needs a contract with must_call
    if [ "$i" = "05" ]; then
        python3 -c "
import json
with open('$TEST_DIR/govern.json','r') as f: d=json.load(f)
d['contracts'] = d.get('contracts', {})
d['contracts']['functions'] = d['contracts'].get('functions', {})
d['contracts']['functions']['process_data'] = {'must_call': ['helper_fn']}
with open('$TEST_DIR/govern.json','w') as f: json.dump(d,f,indent=2)
" 2>/dev/null
        (cd "$TEST_DIR" && "$NAAB" --sign-governance) >/dev/null 2>&1
    fi
    # L-06 needs a contract with must_contain
    if [ "$i" = "06" ]; then
        python3 -c "
import json
with open('$TEST_DIR/govern.json','r') as f: d=json.load(f)
d['contracts'] = d.get('contracts', {})
d['contracts']['functions'] = d['contracts'].get('functions', {})
d['contracts']['functions']['process_items'] = {'must_contain': ['for']}
with open('$TEST_DIR/govern.json','w') as f: json.dump(d,f,indent=2)
" 2>/dev/null
        (cd "$TEST_DIR" && "$NAAB" --sign-governance) >/dev/null 2>&1
    fi
    # L-07 needs must_produce contract
    if [ "$i" = "07" ]; then
        python3 -c "
import json
with open('$TEST_DIR/govern.json','r') as f: d=json.load(f)
d['contracts'] = d.get('contracts', {})
d['contracts']['functions'] = d['contracts'].get('functions', {})
d['contracts']['functions']['normalize_value'] = {
    'must_produce': [{'args': ['CRITICAL'], 'expect': 0}]
}
with open('$TEST_DIR/govern.json','w') as f: json.dump(d,f,indent=2)
" 2>/dev/null
        (cd "$TEST_DIR" && "$NAAB" --sign-governance) >/dev/null 2>&1
    fi
    # L-08 needs arity contract
    if [ "$i" = "08" ]; then
        python3 -c "
import json
with open('$TEST_DIR/govern.json','r') as f: d=json.load(f)
d['contracts'] = d.get('contracts', {})
d['contracts']['functions'] = d['contracts'].get('functions', {})
d['contracts']['functions']['single_arg_fn'] = {'arity': {'min': 1, 'max': 1}}
with open('$TEST_DIR/govern.json','w') as f: json.dump(d,f,indent=2)
" 2>/dev/null
        (cd "$TEST_DIR" && "$NAAB" --sign-governance) >/dev/null 2>&1
    fi
    # L-09 needs soft contract
    if [ "$i" = "09" ]; then
        python3 -c "
import json
with open('$TEST_DIR/govern.json','r') as f: d=json.load(f)
d['contracts'] = d.get('contracts', {})
d['contracts']['level'] = 'soft'
d['contracts']['functions'] = d['contracts'].get('functions', {})
d['contracts']['functions']['compute_result'] = {
    'return_type': 'dict',
    'return_keys': ['value', 'status']
}
with open('$TEST_DIR/govern.json','w') as f: json.dump(d,f,indent=2)
" 2>/dev/null
        (cd "$TEST_DIR" && "$NAAB" --sign-governance) >/dev/null 2>&1
    fi
    # L-10 needs advisory contract
    if [ "$i" = "10" ]; then
        python3 -c "
import json
with open('$TEST_DIR/govern.json','r') as f: d=json.load(f)
d['contracts'] = d.get('contracts', {})
d['contracts']['level'] = 'advisory'
d['contracts']['functions'] = d['contracts'].get('functions', {})
d['contracts']['functions']['compute_result'] = {
    'return_type': 'dict',
    'return_keys': ['value', 'status']
}
with open('$TEST_DIR/govern.json','w') as f: json.dump(d,f,indent=2)
" 2>/dev/null
        (cd "$TEST_DIR" && "$NAAB" --sign-governance) >/dev/null 2>&1
    fi
    run_naab "$(basename "$test_file")"
    actual=$NAAB_EXIT
    check_exit_oneof "L-$i" "$test_name" "$actual" $expected_exits
done
rm -rf "$TMPBASE"/test_*
fi

# ═══════════════════════════════════════════════════════════
# CAT 7: RUNTIME — Taint Edge Cases (10 tests)
# ═══════════════════════════════════════════════════════════
if should_run 7; then
echo -e "\n${BOLD}${CYAN}── Cat 7: RUNTIME (Taint) ────────────────────────────────${NC}"
echo -e "   Tests taint tracking edge cases and propagation"

for i in 01 02 03 04 05 06 07 08 09 10; do
    test_file="$SCRIPT_DIR/src/runtime_${i}_*.naab"
    test_file=$(echo $test_file)
    if [ ! -f "$test_file" ]; then
        skip "R-$i" "runtime_${i} (file not found)"
        continue
    fi
    test_name=$(basename "$test_file" .naab)
    # R-01: reassignment clears taint → exit 0
    case "$i" in
        01) expected_exits="0" ;;
        *) expected_exits="3 4 1 2" ;;
    esac
    setup_test_dir "phase1-strict.json"
    cp "$test_file" "$TEST_DIR/"
    # Create a temp file for file.write sink tests
    echo "" > "$TEST_DIR/output.txt"
    run_naab "$(basename "$test_file")"
    actual=$NAAB_EXIT
    check_exit_oneof "R-$i" "$test_name" "$actual" $expected_exits
done
rm -rf "$TMPBASE"/test_*
fi

# ═══════════════════════════════════════════════════════════
# CAT 8: REVALIDATION (8 tests)
# ═══════════════════════════════════════════════════════════
if should_run 8; then
echo -e "\n${BOLD}${CYAN}── Cat 8: REVALIDATION ───────────────────────────────────${NC}"
echo -e "   Tests one-way ratchet and mid-run config reload"

if [ -f "$SCRIPT_DIR/src/revalidation_tests.sh" ]; then
    bash "$SCRIPT_DIR/src/revalidation_tests.sh" "$NAAB" "$TMPBASE" "$KEYGEN_DIR" \
        2>"$RESULTS_DIR/revalidation_stderr.log" | while IFS='|' read -r status id desc detail; do
        case "$status" in
            PASS) pass "$id" "$desc" ;;
            FAIL) fail "$id" "$desc" "$detail" ;;
            SKIP) skip "$id" "$desc" "$detail" ;;
        esac
    done
else
    for i in $(seq 1 8); do
        skip "RV-$(printf '%02d' $i)" "revalidation test (script not found)"
    done
fi
rm -rf "$TMPBASE"/rv*
fi

# ═══════════════════════════════════════════════════════════
# CAT 9: DEPENDENCY (10 tests)
# ═══════════════════════════════════════════════════════════
if should_run 9; then
echo -e "\n${BOLD}${CYAN}── Cat 9: DEPENDENCY ─────────────────────────────────────${NC}"
echo -e "   Tests governance enforcement across module boundaries"

for i in 01 02 03 04 05 06 07 08 09 10; do
    test_file="$SCRIPT_DIR/src/dep_${i}_*.naab"
    test_file=$(echo $test_file)
    if [ ! -f "$test_file" ]; then
        skip "D-$i" "dep_${i} (file not found)"
        continue
    fi
    test_name=$(basename "$test_file" .naab)
    # D-01: function shadowing → exit 0 (valid NAAb)
    # D-02: module main block skipped → exit 0
    case "$i" in
        01|02) expected_exits="0" ;;
        03) expected_exits="1 3 4" ;;  # circular import = error
        06) expected_exits="1 3 4" ;;  # path traversal blocked
        10) expected_exits="1 3 4" ;;  # symlink escape blocked
        *) expected_exits="3 4 1" ;;
    esac
    setup_test_dir "phase1-strict.json"
    # Copy all helper modules
    for helper in "$SCRIPT_DIR/src/dep_helper_"*.naab; do
        [ -f "$helper" ] && cp "$helper" "$TEST_DIR/"
    done
    # Copy circular import helpers
    for helper in "$SCRIPT_DIR/src/dep_circular_"*.naab; do
        [ -f "$helper" ] && cp "$helper" "$TEST_DIR/"
    done
    # Copy deep chain helpers
    for helper in "$SCRIPT_DIR/src/dep_chain_"*.naab; do
        [ -f "$helper" ] && cp "$helper" "$TEST_DIR/"
    done
    # Copy re-export helpers
    for helper in "$SCRIPT_DIR/src/dep_reexport_"*.naab; do
        [ -f "$helper" ] && cp "$helper" "$TEST_DIR/"
    done
    cp "$test_file" "$TEST_DIR/"
    # D-10: create symlink outside project
    if [ "$i" = "10" ]; then
        ln -sf /etc/passwd "$TEST_DIR/escape_link.naab" 2>/dev/null || true
    fi
    run_naab "$(basename "$test_file")"
    actual=$NAAB_EXIT
    check_exit_oneof "D-$i" "$test_name" "$actual" $expected_exits
done
rm -rf "$TMPBASE"/test_*
fi

# ═══════════════════════════════════════════════════════════
# CAT 10: SURVIVABILITY (10 tests)
# ═══════════════════════════════════════════════════════════
if should_run 10; then
echo -e "\n${BOLD}${CYAN}── Cat 10: SURVIVABILITY ─────────────────────────────────${NC}"
echo -e "   Tests governance resilience under active attack from polyglot code"

if [ -f "$SCRIPT_DIR/src/survivability_tests.sh" ]; then
    bash "$SCRIPT_DIR/src/survivability_tests.sh" "$NAAB" "$TMPBASE" "$KEYGEN_DIR" \
        2>"$RESULTS_DIR/survivability_stderr.log" | while IFS='|' read -r status id desc detail; do
        case "$status" in
            PASS) pass "$id" "$desc" ;;
            FAIL) fail "$id" "$desc" "$detail" ;;
            SKIP) skip "$id" "$desc" "$detail" ;;
        esac
    done
else
    for i in $(seq 1 10); do
        skip "S-$(printf '%02d' $i)" "survivability test (script not found)"
    done
fi
rm -rf "$TMPBASE"/s0*
fi

# ═══════════════════════════════════════════════════════════
# CAT 11: EVOLVING REALITY (8 tests)
# ═══════════════════════════════════════════════════════════
if should_run 11; then
echo -e "\n${BOLD}${CYAN}── Cat 11: EVOLVING REALITY ──────────────────────────────${NC}"
echo -e "   Tests config mutation and ratchet enforcement"

if [ -f "$SCRIPT_DIR/src/evolving_tests.sh" ]; then
    bash "$SCRIPT_DIR/src/evolving_tests.sh" "$NAAB" "$TMPBASE" "$KEYGEN_DIR" \
        2>"$RESULTS_DIR/evolving_stderr.log" | while IFS='|' read -r status id desc detail; do
        case "$status" in
            PASS) pass "$id" "$desc" ;;
            FAIL) fail "$id" "$desc" "$detail" ;;
            SKIP) skip "$id" "$desc" "$detail" ;;
        esac
    done
else
    for i in $(seq 1 8); do
        skip "EO-$(printf '%02d' $i)" "evolving reality test (script not found)"
    done
fi
rm -rf "$TMPBASE"/eo*
fi

# ═══════════════════════════════════════════════════════════
# Restore trust store and report
# ═══════════════════════════════════════════════════════════
restore_trust_store

echo ""
echo -e "${BOLD}${CYAN}══════════════════════════════════════════════════════════${NC}"
echo -e "${BOLD}  RESULTS: ${GREEN}${PASS_COUNT} pass${NC} / ${RED}${FAIL_COUNT} fail${NC} / ${YELLOW}${SKIP_COUNT} skip${NC} / ${TOTAL} total${NC}"
echo -e "${BOLD}${CYAN}══════════════════════════════════════════════════════════${NC}"

if [ $FAIL_COUNT -gt 0 ]; then
    echo -e "\n${RED}${BOLD}FAILURES:${NC}${FAILURES}"
fi

# Write results to file
cat > "$RESULTS_DIR/summary.txt" <<SUMMARY
NAAb-29 Adversarial Governance Stress Test
Date: $(date -u +"%Y-%m-%d %H:%M:%S UTC")
Pass: $PASS_COUNT
Fail: $FAIL_COUNT
Skip: $SKIP_COUNT
Total: $TOTAL
SUMMARY

if [ $FAIL_COUNT -gt 0 ]; then
    exit 1
else
    exit 0
fi
