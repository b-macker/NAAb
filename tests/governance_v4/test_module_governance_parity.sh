#!/usr/bin/env bash
# ============================================================
# test_module_governance_parity.sh — VM vs tree-walker module governance agreement
#
# CLAUDE.md claims: "VM compiler skips function-body governance checks during
# module loading (`!skip_main_` guard in compiler.cpp), matching tree-walker's
# `module_loading_depth_ == 0` guard."
#
# What this actually means:
#   - checkNaabFunctionBody() (full: placeholders, oversimplification,
#     complexity floor, secrets, PII) runs ONLY for the main program
#   - checkFunctionBehavioralContract() (lightweight: must_call, must_contain,
#     arity) runs for BOTH modules and main program
#   - The main{} block is NOT executed during module import
#
# This test verifies all three properties on both engines:
#   A. A placeholder in a MODULE function body must NOT trigger a governance block
#   B. The same placeholder in a MAIN-FILE function body MUST trigger a block
#   C. A must_call contract on a module function MUST still fire during import
#   D. Main block in a module must NOT execute during import
#
# Every assertion is backed by:
#   - Engine parity: VM and tree-walker produce the same verdict
#   - Correctness: the agreed verdict is the expected one
#   - Controls: the checks are actually active (not vacuously passing)
#
# Traces to: compiler.cpp:89/97/399/1129/1267 (skip_main_)
#            interpreter.cpp:975/982/995/1027/1032/1037/1067/1151 (module_loading_depth_)
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/module-gov-parity-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

# POSIX-only guard (same pattern as test_sandbox_engine_parity.sh)
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        echo ""
        echo "  test_module_governance_parity.sh: SKIPPED (POSIX-only —"
        echo "    absolute-path probes are not meaningful for a native binary under MSYS2)"
        exit 0 ;;
esac
if [ -n "${WINDIR:-}" ]; then
    echo "  test_module_governance_parity.sh: SKIPPED (POSIX-only)"
    exit 0
fi

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
cleanup() { teardown_isolated_trust; rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"

"$NAAB" --keygen "$TEST_TMP/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/k.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/k.pem"

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  Module governance: VM vs tree-walker must agree             |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

write_govern() {
    printf '%s\n' "$1" > "$TEST_TMP/govern.json"
    (cd "$TEST_TMP" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
}

# run_probe FILE ENGINE_FLAG -> captured combined stdout+stderr
run_probe() {
    (cd "$TEST_TMP" && timeout 30s "$NAAB" ${2:-} "$1" 2>&1) || true
}

# check_parity ID EXPECTED DESCRIPTION FILE [ENGINE_FLAG_EXTRA]
# Runs both engines, checks they agree AND produce the expected verdict.
# EXPECTED is a grep -iE pattern for "what the output should contain".
# If EXPECTED starts with "!" then the output must NOT contain the rest.
check_parity() {
    local id="$1" expected="$2" desc="$3" file="$4"
    local vm_out tw_out vm_match tw_match negate=false

    if [[ "$expected" == !* ]]; then
        negate=true
        expected="${expected:1}"
    fi

    vm_out=$(run_probe "$file" "")
    tw_out=$(run_probe "$file" "--tree-walk")

    vm_match=$(echo "$vm_out" | grep -ciE "$expected" || true)
    tw_match=$(echo "$tw_out" | grep -ciE "$expected" || true)

    if [ "$negate" = true ]; then
        # Neither engine should match
        local vm_ok=false tw_ok=false
        [ "$vm_match" -eq 0 ] && vm_ok=true
        [ "$tw_match" -eq 0 ] && tw_ok=true

        if [ "$vm_ok" = true ] && [ "$tw_ok" = true ]; then
            pass "$id" "$desc"
        elif [ "$vm_ok" != "$tw_ok" ]; then
            fail "$id" "engines disagree: $desc" \
                "VM matches=$vm_match tree-walk matches=$tw_match"
        else
            fail "$id" "both engines gave wrong verdict: $desc" \
                "expected no match, both matched ($vm_match/$tw_match)"
        fi
    else
        # Both engines should match
        local vm_ok=false tw_ok=false
        [ "$vm_match" -gt 0 ] && vm_ok=true
        [ "$tw_match" -gt 0 ] && tw_ok=true

        if [ "$vm_ok" = true ] && [ "$tw_ok" = true ]; then
            pass "$id" "$desc"
        elif [ "$vm_ok" != "$tw_ok" ]; then
            fail "$id" "engines disagree: $desc" \
                "VM matches=$vm_match tree-walk matches=$tw_match"
        else
            fail "$id" "both engines gave wrong verdict: $desc" \
                "expected match, neither matched (VM=$vm_match TW=$tw_match)"
        fi
    fi
}

# Also check exit codes agree
check_exit_parity() {
    local id="$1" expected_rc="$2" desc="$3" file="$4"
    local vm_rc tw_rc

    (cd "$TEST_TMP" && timeout 30s "$NAAB" "$file" >/dev/null 2>&1) && vm_rc=$? || vm_rc=$?
    (cd "$TEST_TMP" && timeout 30s "$NAAB" --tree-walk "$file" >/dev/null 2>&1) && tw_rc=$? || tw_rc=$?

    if [ "$vm_rc" != "$tw_rc" ]; then
        fail "$id" "exit code divergence: $desc" "VM=$vm_rc tree-walk=$tw_rc"
    elif [ "$vm_rc" != "$expected_rc" ]; then
        fail "$id" "both engines gave wrong exit code: $desc" \
            "expected $expected_rc, both gave $vm_rc"
    else
        pass "$id" "$desc (exit $vm_rc)"
    fi
}

# ---------------------------------------------------------------------------
# Govern.json: placeholders=HARD, must_call enabled, require_main_block=off
# ---------------------------------------------------------------------------
write_govern '{
    "version": "5.0",
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "restrictions": {
        "no_placeholders": "hard"
    },
    "behavioral_contracts": {
        "must_call": {
            "module_greet": ["string"]
        }
    }
}'

# ---------------------------------------------------------------------------
# Group A: Module with a placeholder — must NOT be blocked
# ---------------------------------------------------------------------------
echo "--- Group A: Module import skips full body governance ---"

# Module file: exported function with a TODO placeholder
cat > "$TEST_TMP/mod_placeholder.naab" << 'NAAB'
// A module with a placeholder in its function body
export fn greet(name) {
    // TODO: add proper greeting logic
    return "Hello, " + name
}
NAAB

# Main file that imports the module and uses it
cat > "$TEST_TMP/test_a_import.naab" << 'NAAB'
import "mod_placeholder.naab" as m

main {
    let result = m.greet("World")
    print("IMPORT_OK:" + result)
}
NAAB

# The placeholder in the module must not trigger a governance block.
# Governance errors emit "no_placeholders" or "placeholder" in the message.
check_parity "MOD-A1" "!placeholder|no_placeholder" \
    "module function placeholder is NOT blocked on import" \
    "test_a_import.naab"

# And the import must succeed
check_parity "MOD-A2" "IMPORT_OK" \
    "module import succeeds despite placeholder in module body" \
    "test_a_import.naab"

check_exit_parity "MOD-A3" "0" \
    "importing a module with placeholder exits 0" \
    "test_a_import.naab"

# ---------------------------------------------------------------------------
# Group B: Main file with same placeholder — MUST be blocked
# ---------------------------------------------------------------------------
echo ""
echo "--- Group B: Main file triggers full body governance ---"

# Same placeholder in the main file's own function
cat > "$TEST_TMP/test_b_main.naab" << 'NAAB'
fn greet(name) {
    // TODO: add proper greeting logic
    return "Hello, " + name
}

main {
    let result = greet("World")
    print("MAIN_OK:" + result)
}
NAAB

# The placeholder in the main file MUST trigger a governance block.
check_parity "MOD-B1" "placeholder" \
    "main-file function placeholder IS blocked" \
    "test_b_main.naab"

# And it should NOT print the success message
check_parity "MOD-B2" "!MAIN_OK" \
    "main-file function is blocked before execution reaches print" \
    "test_b_main.naab"

# Exit code should be non-zero (governance block)
# HARD governance = exit 3; some paths may exit 1 — check non-zero at minimum
check_exit_parity "MOD-B3" "3" \
    "HARD placeholder block exits 3" \
    "test_b_main.naab"

# ---------------------------------------------------------------------------
# Group C: Behavioral contract fires DURING module loading
# ---------------------------------------------------------------------------
echo ""
echo "--- Group C: Behavioral contracts still fire on modules ---"

# Update govern.json to add a must_call contract for module function
write_govern '{
    "version": "5.0",
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "contracts": {
        "level": "hard",
        "functions": {
            "compute": {
                "must_call": ["math.sqrt"]
            }
        }
    }
}'

# Module where the function violates must_call
cat > "$TEST_TMP/mod_contract_bad.naab" << 'NAAB'
export fn compute(x) {
    return x * 2
}
NAAB

cat > "$TEST_TMP/test_c_contract.naab" << 'NAAB'
import "mod_contract_bad.naab" as m

main {
    let v = m.compute(4)
    print("CONTRACT_OK:" + string(v))
}
NAAB

# must_call violation should trigger even during module loading
check_parity "MOD-C1" "must_call|behavioral_contract|must call" \
    "must_call contract fires during module load" \
    "test_c_contract.naab"

check_parity "MOD-C2" "!CONTRACT_OK" \
    "must_call blocks before main executes" \
    "test_c_contract.naab"

# Control: module that satisfies the contract should pass
cat > "$TEST_TMP/mod_contract_ok.naab" << 'NAAB'
use math
export fn compute(x) {
    return math.sqrt(x)
}
NAAB

cat > "$TEST_TMP/test_c_ok.naab" << 'NAAB'
import "mod_contract_ok.naab" as m

main {
    let v = m.compute(4)
    print("CONTRACT_PASS:" + string(v))
}
NAAB

check_parity "MOD-C3" "CONTRACT_PASS" \
    "control: satisfying must_call lets module load succeed" \
    "test_c_ok.naab"

# ---------------------------------------------------------------------------
# Group D: Module main{} block is NOT executed during import
# ---------------------------------------------------------------------------
echo ""
echo "--- Group D: Module main block skipped on import ---"

write_govern '{
    "version": "5.0",
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" }
}'

cat > "$TEST_TMP/mod_with_main.naab" << 'NAAB'
export fn helper() {
    return 42
}

main {
    print("MODULE_MAIN_EXECUTED")
}
NAAB

cat > "$TEST_TMP/test_d_main.naab" << 'NAAB'
import "mod_with_main.naab" as m

main {
    let v = m.helper()
    print("IMPORT_MAIN_SKIP:" + string(v))
}
NAAB

# The module's main{} must NOT run
check_parity "MOD-D1" "!MODULE_MAIN_EXECUTED" \
    "module main block does NOT execute on import" \
    "test_d_main.naab"

# The importer's main MUST run
check_parity "MOD-D2" "IMPORT_MAIN_SKIP" \
    "importer main block runs normally" \
    "test_d_main.naab"

check_exit_parity "MOD-D3" "0" \
    "importing a module with its own main exits 0" \
    "test_d_main.naab"

# ---------------------------------------------------------------------------
# Group E: Control — placeholder check is actually active
# ---------------------------------------------------------------------------
echo ""
echo "--- Group E: Controls (checks are live, not vacuously passing) ---"

write_govern '{
    "version": "5.0",
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "restrictions": {
        "no_placeholders": "hard"
    }
}'

# A main-file function with a different placeholder pattern
cat > "$TEST_TMP/test_e_control.naab" << 'NAAB'
fn process(data) {
    // FIXME: implement real processing
    return data
}

main {
    print(process("test"))
}
NAAB

check_parity "MOD-E1" "placeholder" \
    "control: FIXME also triggers placeholder check (check is live)" \
    "test_e_control.naab"

# With placeholders disabled, the same code should run — proves the block
# above was governance, not a parse error.
write_govern '{
    "version": "5.0",
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "restrictions": {
        "no_placeholders": false
    }
}'

cat > "$TEST_TMP/test_e_nogov.naab" << 'NAAB'
fn greet(name) {
    // TODO: add proper greeting logic
    return "Hello, " + name
}

main {
    print("NOGOV_OK:" + greet("World"))
}
NAAB

VM_NOGOV=$(cd "$TEST_TMP" && timeout 30s "$NAAB" test_e_nogov.naab 2>&1) || true
if echo "$VM_NOGOV" | grep -q "NOGOV_OK"; then
    pass "MOD-E2" "control: disabling no_placeholders lets the code through (block is governance, not syntax)"
else
    fail "MOD-E2" "control failed: code doesn't run even with placeholders disabled" \
        "output: ${VM_NOGOV:0:200}"
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo ""
echo -e "${CYAN}+==============================================================+${NC}"
TOTAL=$((PASS_COUNT + FAIL_COUNT + SKIP_COUNT))
echo -e "  Total: $TOTAL | ${GREEN}Pass: $PASS_COUNT${NC} | ${RED}Fail: $FAIL_COUNT${NC} | ${YELLOW}Skip: $SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then
    echo -e "${RED}Failures:${NC}$FAILURES"
    exit 1
fi
exit 0
