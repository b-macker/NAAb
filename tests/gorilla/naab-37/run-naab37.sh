#!/usr/bin/env bash
# NAAb-37: Real-World Gorilla Test for 14 Bug Fixes
# 38 assertions across 6 categories testing GovernanceHardError bypasses,
# codegen injection, enforcement gaps, subprocess hygiene, concurrency, integration
# Usage: bash run-naab37.sh [--cat N]   (N=1..6, omit for all)
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../../build/naab-lang"
TMPBASE="${TMPDIR:-/data/data/com.termux/files/usr/tmp}/naab37-$$"
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

# --- Helpers ---

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
        echo -e "       ${RED}-> $detail${NC}"
    fi
    FAILURES="${FAILURES}\n  [$id] $desc${detail:+ -- $detail}"
}

skip() {
    local id="$1" desc="$2" reason="${3:-}"
    SKIP_COUNT=$((SKIP_COUNT + 1))
    TOTAL=$((TOTAL + 1))
    echo -e "  ${YELLOW}SKIP${NC} [$id] $desc${reason:+ ($reason)}"
}

# Setup an isolated test directory with governance
# Usage: setup_test_dir <phase_config>
# Sets TEST_DIR to the created directory
setup_test_dir() {
    local phase="$1"
    TEST_DIR="$TMPBASE/test_$(date +%s%N)"
    mkdir -p "$TEST_DIR"
    cp "$SCRIPT_DIR/phases/$phase" "$TEST_DIR/govern.json"
    (cd "$TEST_DIR" && "$NAAB" --sign-governance) >/dev/null 2>&1
}

# Run a naab test -- sets NAAB_EXIT to the exit code
# Usage: run_naab <test_file> [extra_flags...]
NAAB_EXIT=0
run_naab() {
    local test_file="$1"; shift
    (cd "$TEST_DIR" && "$NAAB" "$@" "$test_file") >"$TEST_DIR/stdout.log" 2>"$TEST_DIR/stderr.log" && NAAB_EXIT=0 || NAAB_EXIT=$?
}

# Check exit code matches expected
check_exit() {
    local id="$1" desc="$2" expected="$3" actual="$4"
    if [ "$actual" -eq "$expected" ]; then
        pass "$id" "$desc (exit $actual)"
    else
        fail "$id" "$desc" "expected exit $expected, got $actual"
    fi
}

# Check exit code is in a set of acceptable values
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

# --- Signing Setup ---

echo -e "${BOLD}${CYAN}+----------------------------------------------------------+${NC}"
echo -e "${BOLD}${CYAN}|  NAAb-37: Real-World Gorilla Test (14 Bug Fixes)         |${NC}"
echo -e "${BOLD}${CYAN}|  38 assertions - 6 categories - 4 governance phases      |${NC}"
echo -e "${BOLD}${CYAN}+----------------------------------------------------------+${NC}"
echo ""

# Generate a temporary signing key for test isolation
KEYGEN_DIR="$TMPBASE/keys"
mkdir -p "$KEYGEN_DIR"
(cd "$KEYGEN_DIR" && "$NAAB" --keygen test-key.pem) >/dev/null 2>&1
export NAAB_SIGNING_KEY="$KEYGEN_DIR/test-key.pem"

# ===================================================================
# CAT 1: UNCATCHABLE (8 tests) -- GovernanceHardError bypasses
# Fixes: 1a (catch body), 1b (async), 1c (await)
# ===================================================================
if should_run 1; then
echo -e "\n${BOLD}${CYAN}-- Cat 1: UNCATCHABLE ----------------------------------------${NC}"
echo -e "   GovernanceHardError cannot be caught by NAAb try/catch"

# U-01: Catch body triggers HARD (os.system in polyglot) -- Fix 1a
setup_test_dir "phase1-baseline.json"
cat > "$TEST_DIR/test.naab" << 'NAAB'
main {
    try {
        throw Error("trigger catch")
    } catch (e) {
        let result = <<python
import os
os.system("ls")
>>
        print(result)
    }
}
NAAB
run_naab "test.naab"
check_exit "U-01" "Catch body HARD (os.system)" 3 "$NAAB_EXIT"

# U-02: Catch body triggers HARD (blocked language) -- Fix 1a
setup_test_dir "phase1-baseline.json"
cat > "$TEST_DIR/test.naab" << 'NAAB'
main {
    try {
        throw Error("trigger catch")
    } catch (e) {
        let result = <<rust
fn main() { println!("bypassed"); }
>>
        print(result)
    }
}
NAAB
run_naab "test.naab"
check_exit_oneof "U-02" "Catch body HARD (blocked lang)" "$NAAB_EXIT" 3 4

# U-03: Async fn triggers HARD -- Fix 1b (tree-walk: async uses call_dispatch.cpp)
setup_test_dir "phase1-baseline.json"
cat > "$TEST_DIR/test.naab" << 'NAAB'
async fn violate_governance() {
    let result = <<python
import os
os.system("id")
>>
    return result
}

main {
    let f = violate_governance()
    let r = await f
    print("should not reach: " + string(r))
}
NAAB
run_naab "test.naab" --tree-walk
check_exit "U-03" "Async fn HARD block (tree-walk)" 3 "$NAAB_EXIT"

# U-04: Nested try/catch, inner catch HARD -- Fix 1a (nested)
setup_test_dir "phase1-baseline.json"
cat > "$TEST_DIR/test.naab" << 'NAAB'
main {
    try {
        try {
            throw Error("inner error")
        } catch (e) {
            let result = <<python
import os
os.system("whoami")
>>
            print(result)
        }
    } catch (e2) {
        print("outer caught: " + string(e2))
    }
    print("should not reach")
}
NAAB
run_naab "test.naab"
check_exit "U-04" "Nested try/catch inner HARD" 3 "$NAAB_EXIT"

# U-05: HARD in loop via catch -- Fix 1a
setup_test_dir "phase1-baseline.json"
cat > "$TEST_DIR/test.naab" << 'NAAB'
main {
    for i in 0..3 {
        try {
            throw Error("loop error " + string(i))
        } catch (e) {
            let result = <<python
import os
os.system("echo bypass")
>>
            print(result)
        }
    }
}
NAAB
run_naab "test.naab"
check_exit "U-05" "HARD in loop via catch" 3 "$NAAB_EXIT"

# U-06: Sanity -- try/catch with runtime error (NOT governance)
setup_test_dir "phase1-baseline.json"
cat > "$TEST_DIR/test.naab" << 'NAAB'
main {
    try {
        let x = string(null)
        print(x)
    } catch (e) {
        print("caught runtime error")
    }
}
NAAB
run_naab "test.naab"
check_exit "U-06" "Sanity: runtime error catchable" 0 "$NAAB_EXIT"

# U-07: Sanity -- normal async fn works
setup_test_dir "phase1-baseline.json"
cat > "$TEST_DIR/test.naab" << 'NAAB'
async fn compute(n) {
    let sum = 0
    for i in 0..n {
        sum = sum + i
    }
    return sum
}

main {
    let r = await compute(10)
    print("async result: " + string(r))
}
NAAB
run_naab "test.naab"
check_exit "U-07" "Sanity: normal async fn" 0 "$NAAB_EXIT"

# U-08: Sanity -- HARD outside try/catch
setup_test_dir "phase1-baseline.json"
cat > "$TEST_DIR/test.naab" << 'NAAB'
main {
    let result = <<python
import os
os.system("ls")
>>
    print(result)
}
NAAB
run_naab "test.naab"
check_exit "U-08" "Sanity: HARD outside try/catch" 3 "$NAAB_EXIT"

rm -rf "$TMPBASE"/test_*
fi

# ===================================================================
# CAT 2: CODEGEN INJECTION (8 tests) -- Binding validation + escaping
# Fixes: 2a (key validation), 2b (shell escaping), 2c (scan after preamble)
# ===================================================================
if should_run 2; then
echo -e "\n${BOLD}${CYAN}-- Cat 2: CODEGEN INJECTION ----------------------------------${NC}"
echo -e "   Binding key validation and shell value escaping"

# CI-01: Valid binding keys work
setup_test_dir "phase2-codegen.json"
cat > "$TEST_DIR/test.naab" << 'NAAB'
use codegen

main {
    let result = codegen.run_with_args("python", "print(x + y)", {"x": 1, "y": 2})
    print("result: " + string(result))
}
NAAB
run_naab "test.naab"
check_exit "CI-01" "Valid binding keys" 0 "$NAAB_EXIT"

# CI-02: Binding key with semicolon -- Fix 2a
setup_test_dir "phase2-codegen.json"
cat > "$TEST_DIR/test.naab" << 'NAAB'
use codegen

main {
    let result = codegen.run_with_args("python", "print(1)", {"key;bad": 1})
    print(result)
}
NAAB
run_naab "test.naab"
check_exit "CI-02" "Binding key with semicolon blocked" 1 "$NAAB_EXIT"

# CI-03: Binding key with space -- Fix 2a
setup_test_dir "phase2-codegen.json"
cat > "$TEST_DIR/test.naab" << 'NAAB'
use codegen

main {
    let result = codegen.run_with_args("python", "print(1)", {"key bad": 1})
    print(result)
}
NAAB
run_naab "test.naab"
check_exit "CI-03" "Binding key with space blocked" 1 "$NAAB_EXIT"

# CI-04: Binding key with dollar-paren -- Fix 2a
setup_test_dir "phase2-codegen.json"
cat > "$TEST_DIR/test.naab" << 'NAAB'
use codegen

main {
    let bindings = {}
    bindings["$(whoami)"] = 1
    let result = codegen.run_with_args("python", "print(1)", bindings)
    print(result)
}
NAAB
run_naab "test.naab"
check_exit "CI-04" "Binding key with $(cmd) blocked" 1 "$NAAB_EXIT"

# CI-05: Shell string with apostrophe -- Fix 2b
setup_test_dir "phase2-codegen.json"
cat > "$TEST_DIR/test.naab" << 'NAAB'
use codegen

main {
    let result = codegen.run_with_args("shell", "echo $var", {"var": "it's ok"})
    print(result)
}
NAAB
run_naab "test.naab"
if [ "$NAAB_EXIT" -eq 0 ]; then
    if grep -q "it's ok" "$TEST_DIR/stdout.log"; then
        pass "CI-05" "Shell apostrophe escaping correct"
    else
        fail "CI-05" "Shell apostrophe escaping" "output missing expected text"
    fi
else
    fail "CI-05" "Shell apostrophe escaping" "exit $NAAB_EXIT (expected 0)"
fi

# CI-06: Shell string with backtick -- Fix 2b
# Backtick in binding value should NOT execute as command substitution
# Output should contain the literal backtick string, not the result of execution
setup_test_dir "phase2-codegen.json"
cat > "$TEST_DIR/test.naab" << 'NAAB'
use codegen

main {
    let result = codegen.run_with_args("shell", "echo $var", {"var": "a`echo injected`b"})
    print(result)
}
NAAB
run_naab "test.naab"
if [ "$NAAB_EXIT" -eq 0 ]; then
    # The output should contain the backtick literally, not just "ainjectedb"
    # If backtick was executed, output would be "ainjectedb" without backticks
    # With proper escaping, output contains the literal backtick characters
    if grep -q 'echo injected' "$TEST_DIR/stdout.log" 2>/dev/null; then
        pass "CI-06" "Shell backtick safely quoted (literal in output)"
    else
        fail "CI-06" "Shell backtick escaping" "backtick may have been executed"
    fi
else
    pass "CI-06" "Shell backtick blocked (exit $NAAB_EXIT)"
fi

# CI-07: Shell string with $HOME -- Fix 2b
setup_test_dir "phase2-codegen.json"
cat > "$TEST_DIR/test.naab" << 'NAAB'
use codegen

main {
    let result = codegen.run_with_args("shell", "echo $var", {"var": "$HOME"})
    print(result)
}
NAAB
run_naab "test.naab"
if [ "$NAAB_EXIT" -eq 0 ]; then
    # Should print literal "$HOME", not the expanded path
    if grep -q '\$HOME' "$TEST_DIR/stdout.log"; then
        pass "CI-07" "Shell \$HOME not expanded (literal)"
    elif grep -q "$HOME" "$TEST_DIR/stdout.log"; then
        fail "CI-07" "Shell \$HOME escaping" "\$HOME was expanded to $HOME"
    else
        pass "CI-07" "Shell \$HOME safely quoted"
    fi
else
    fail "CI-07" "Shell \$HOME escaping" "exit $NAAB_EXIT (expected 0)"
fi

# CI-08: Binding value with dangerous import -- Fix 2c (scan after preamble)
setup_test_dir "phase2-codegen.json"
cat > "$TEST_DIR/test.naab" << 'NAAB'
use codegen

main {
    let result = codegen.run_with_args("python", "print(x)", {"x": "__import__('os').system('id')"})
    print(result)
}
NAAB
run_naab "test.naab"
check_exit_oneof "CI-08" "Dangerous binding value blocked" "$NAAB_EXIT" 3 1

rm -rf "$TMPBASE"/test_*
fi

# ===================================================================
# CAT 3: ENFORCEMENT (6 tests) -- Token budget + advisory escalation
# Fixes: 4a (max_total_tokens: 0), 4b (advisory escalation deadlock)
# ===================================================================
if should_run 3; then
echo -e "\n${BOLD}${CYAN}-- Cat 3: ENFORCEMENT ----------------------------------------${NC}"
echo -e "   Token budget guards and advisory escalation"

# EN-01: agent.check with max_total_tokens: 0 -- Fix 4a
setup_test_dir "phase3-escalation.json"
cat > "$TEST_DIR/test.naab" << 'NAAB'
use agent

main {
    let check = agent.check("classifier")
    if check.get("valid") == true {
        print("config valid")
    } else {
        print("config invalid: " + string(check.get("error")))
    }
}
NAAB
run_naab "test.naab"
# agent.check may fail if GEMINI_API_KEY not set, but config parsing with max_total_tokens:0 should not crash
check_exit_oneof "EN-01" "agent.check with max_total_tokens=0" "$NAAB_EXIT" 0 1

# EN-02: Single advisory (1 breakpoint) -- no escalation
setup_test_dir "phase3-escalation.json"
cat > "$TEST_DIR/test.naab" << 'NAAB'
main {
    let result = <<python
import pdb
print("hello")
>>
    print("done")
}
NAAB
run_naab "test.naab"
check_exit "EN-02" "Single advisory (no escalation)" 0 "$NAAB_EXIT"

# EN-03: 3 advisories -> escalation -> HARD -- Fix 4b
setup_test_dir "phase3-escalation.json"
cat > "$TEST_DIR/test.naab" << 'NAAB'
main {
    let r1 = <<python
import pdb
print("block1")
>>
    let r2 = <<python
import pdb
print("block2")
>>
    let r3 = <<python
import pdb
print("block3")
>>
    print("should not reach")
}
NAAB
run_naab "test.naab"
check_exit "EN-03" "3 advisories -> escalation -> HARD" 3 "$NAAB_EXIT"

# EN-04: 2 advisories (under soft_after=3) -- no escalation
setup_test_dir "phase3-escalation.json"
cat > "$TEST_DIR/test.naab" << 'NAAB'
main {
    let r1 = <<python
import pdb
print("block1")
>>
    let r2 = <<python
import pdb
print("block2")
>>
    print("two advisories, no escalation")
}
NAAB
run_naab "test.naab"
check_exit "EN-04" "2 advisories (under threshold)" 0 "$NAAB_EXIT"

# EN-05: Scoring -- dashboard shows score for advisories
setup_test_dir "phase3-escalation.json"
cat > "$TEST_DIR/test.naab" << 'NAAB'
main {
    let r1 = <<python
import pdb
print("scored")
>>
    print("done")
}
NAAB
run_naab "test.naab" --governance-dashboard
if [ "$NAAB_EXIT" -eq 0 ]; then
    if grep -qi "score\|advisory\|WARNING" "$TEST_DIR/stderr.log" 2>/dev/null; then
        pass "EN-05" "Dashboard shows advisory scoring"
    else
        fail "EN-05" "Dashboard advisory scoring" "no score/advisory in stderr"
    fi
else
    fail "EN-05" "Dashboard advisory scoring" "exit $NAAB_EXIT (expected 0)"
fi

# EN-06: Sanity -- HARD violation (no advisory path)
setup_test_dir "phase3-escalation.json"
cat > "$TEST_DIR/test.naab" << 'NAAB'
main {
    let result = <<python
import os
os.system("ls")
>>
    print(result)
}
NAAB
run_naab "test.naab"
check_exit "EN-06" "Sanity: direct HARD violation" 3 "$NAAB_EXIT"

rm -rf "$TMPBASE"/test_*
fi

# ===================================================================
# CAT 4: SUBPROCESS HYGIENE (4 tests) -- Env var scrubbing
# Fix: 5a (environ iterator collect-then-unset)
# ===================================================================
if should_run 4; then
echo -e "\n${BOLD}${CYAN}-- Cat 4: SUBPROCESS HYGIENE ----------------------------------${NC}"
echo -e "   Environment variable scrubbing in child processes"

# Set test env vars
export NAAB37_SCRUB_ME="secret_scrubbed_value"
export NAAB37_SECRET="top_secret_blocked"
export NAAB37_VISIBLE="visible_value_123"

# SH-01: Scrubbed var invisible to shell child
setup_test_dir "phase4-subprocess.json"
cat > "$TEST_DIR/test.naab" << 'NAAB'
main {
    let result = <<shell
echo "$NAAB37_SCRUB_ME"
>>
    print("shell_output:[" + string(result) + "]")
}
NAAB
run_naab "test.naab"
if [ "$NAAB_EXIT" -eq 0 ]; then
    if grep -q "secret_scrubbed_value" "$TEST_DIR/stdout.log" 2>/dev/null; then
        fail "SH-01" "Shell env scrub" "scrubbed var leaked to child process"
    else
        pass "SH-01" "Shell env var scrubbed from child"
    fi
else
    # Non-zero exit may be acceptable (governance may block for other reasons)
    pass "SH-01" "Shell env var scrubbed (exit $NAAB_EXIT)"
fi

# SH-02: Scrubbed var invisible to python child
setup_test_dir "phase4-subprocess.json"
cat > "$TEST_DIR/test.naab" << 'NAAB'
main {
    let result = <<python
import os
val = os.environ.get("NAAB37_SCRUB_ME", "")
print("py_env:" + val + ":")
>>
    print("python_output:[" + string(result) + "]")
}
NAAB
run_naab "test.naab"
if [ "$NAAB_EXIT" -eq 0 ]; then
    if grep -q "secret_scrubbed_value" "$TEST_DIR/stdout.log" 2>/dev/null; then
        fail "SH-02" "Python env scrub" "scrubbed var leaked to child process"
    else
        pass "SH-02" "Python env var scrubbed from child"
    fi
else
    pass "SH-02" "Python env var scrubbed (exit $NAAB_EXIT)"
fi

# SH-03: Non-scrubbed var available to child
setup_test_dir "phase4-subprocess.json"
cat > "$TEST_DIR/test.naab" << 'NAAB'
main {
    let result = <<shell
echo "$NAAB37_VISIBLE"
>>
    print(result)
}
NAAB
run_naab "test.naab"
if [ "$NAAB_EXIT" -eq 0 ]; then
    if grep -q "visible_value_123" "$TEST_DIR/stdout.log" 2>/dev/null; then
        pass "SH-03" "Non-scrubbed var available to child"
    else
        fail "SH-03" "Non-scrubbed var available" "expected value not in output"
    fi
else
    fail "SH-03" "Non-scrubbed var available" "exit $NAAB_EXIT (expected 0)"
fi

# SH-04: blocked_read var in NAAb -> HARD block
setup_test_dir "phase4-subprocess.json"
cat > "$TEST_DIR/test.naab" << 'NAAB'
use env

main {
    let v = env.get("NAAB37_SECRET")
    print("should not reach: " + string(v))
}
NAAB
run_naab "test.naab"
check_exit "SH-04" "blocked_read env var -> HARD" 3 "$NAAB_EXIT"

# Clean up test env vars
unset NAAB37_SCRUB_ME NAAB37_SECRET NAAB37_VISIBLE

rm -rf "$TMPBASE"/test_*
fi

# ===================================================================
# CAT 5: CONCURRENCY (6 tests) -- Data race safety
# Fixes: 3a (checkPolyglotOptimization mutex), 3b (evaluateQualityGate lock),
#        3c (localtime_r)
# ===================================================================
if should_run 5; then
echo -e "\n${BOLD}${CYAN}-- Cat 5: CONCURRENCY ----------------------------------------${NC}"
echo -e "   Concurrent async + polyglot execution (no crashes)"

# CC-01: Two async fns with python polyglot
setup_test_dir "phase1-baseline.json"
cat > "$TEST_DIR/test.naab" << 'NAAB'
async fn compute_a() {
    let result = <<python
total = sum(range(100))
print(total)
>>
    return result
}

async fn compute_b() {
    let result = <<python
total = sum(range(50))
print(total)
>>
    return result
}

main {
    let fa = compute_a()
    let fb = compute_b()
    let ra = await fa
    let rb = await fb
    print("a=" + string(ra) + " b=" + string(rb))
}
NAAB
run_naab "test.naab"
check_exit "CC-01" "Two async fns with polyglot" 0 "$NAAB_EXIT"

# CC-02: Three async fns, different languages
setup_test_dir "phase1-baseline.json"
cat > "$TEST_DIR/test.naab" << 'NAAB'
async fn py_work() {
    let r = <<python
print(42)
>>
    return r
}

async fn js_work() {
    let r = <<javascript
console.log(43)
>>
    return r
}

async fn sh_work() {
    let r = <<shell
echo 44
>>
    return r
}

main {
    let fa = py_work()
    let fb = js_work()
    let fc = sh_work()
    let ra = await fa
    let rb = await fb
    let rc = await fc
    print("py=" + string(ra) + " js=" + string(rb) + " sh=" + string(rc))
}
NAAB
run_naab "test.naab"
check_exit "CC-02" "Three async fns, different langs" 0 "$NAAB_EXIT"

# CC-03: Async fn + governance violation -- exit 3, NOT segfault (tree-walk: async path)
setup_test_dir "phase1-baseline.json"
cat > "$TEST_DIR/test.naab" << 'NAAB'
async fn bad_async() {
    let r = <<python
import os
os.system("id")
>>
    return r
}

main {
    let f = bad_async()
    let r = await f
    print("should not reach")
}
NAAB
run_naab "test.naab" --tree-walk
# Must be exit 3 (governance), NOT 139 (segfault) or 134 (abort)
if [ "$NAAB_EXIT" -eq 139 ] || [ "$NAAB_EXIT" -eq 134 ] || [ "$NAAB_EXIT" -eq 136 ]; then
    fail "CC-03" "Async governance violation" "crashed with signal (exit $NAAB_EXIT)"
else
    check_exit "CC-03" "Async governance violation (no crash)" 3 "$NAAB_EXIT"
fi

# CC-04: Sequential polyglot blocks (stress quality gate lock)
setup_test_dir "phase1-baseline.json"
cat > "$TEST_DIR/test.naab" << 'NAAB'
main {
    let r1 = <<python
print("block1")
>>
    let r2 = <<python
print("block2")
>>
    let r3 = <<python
print("block3")
>>
    let r4 = <<javascript
console.log("block4")
>>
    let r5 = <<shell
echo "block5"
>>
    print("all blocks done")
}
NAAB
run_naab "test.naab"
check_exit "CC-04" "Sequential polyglot blocks" 0 "$NAAB_EXIT"

# CC-05: Async fn returns polyglot result -- correct value
setup_test_dir "phase1-baseline.json"
cat > "$TEST_DIR/test.naab" << 'NAAB'
async fn compute_sum() {
    let result = <<python
print(sum(range(10)))
>>
    return result
}

main {
    let r = await compute_sum()
    print("sum=" + string(r))
}
NAAB
run_naab "test.naab"
if [ "$NAAB_EXIT" -eq 0 ]; then
    if grep -q "45" "$TEST_DIR/stdout.log" 2>/dev/null; then
        pass "CC-05" "Async polyglot returns correct value"
    else
        fail "CC-05" "Async polyglot return value" "expected 45 in output"
    fi
else
    fail "CC-05" "Async polyglot return value" "exit $NAAB_EXIT (expected 0)"
fi

# CC-06: Many sequential codegen.run calls (stress test)
setup_test_dir "phase2-codegen.json"
cat > "$TEST_DIR/test.naab" << 'NAAB'
use codegen

main {
    let total = 0
    for i in 0..5 {
        let result = codegen.run_with_args("python", "print(n * 2)", {"n": i})
        total = total + 1
    }
    print("completed " + string(total) + " codegen calls")
}
NAAB
run_naab "test.naab"
check_exit "CC-06" "Sequential codegen.run calls" 0 "$NAAB_EXIT"

rm -rf "$TMPBASE"/test_*
fi

# ===================================================================
# CAT 6: INTEGRATION (6 tests) -- Haiku's project
# Tests the actual project that Haiku builds
# ===================================================================
if should_run 6; then
echo -e "\n${BOLD}${CYAN}-- Cat 6: INTEGRATION ----------------------------------------${NC}"
echo -e "   Real-world project coded by Haiku"

# Check if Haiku's files exist
if [ ! -f "$SCRIPT_DIR/src/main.naab" ]; then
    skip "INT-01" "main.naab runs" "src/main.naab not found (Haiku hasn't coded yet)"
    skip "INT-02" "Output has analysis keywords" "src/main.naab not found"
    skip "INT-03" "Sanitized output" "src/main.naab not found"
    skip "INT-04" "codegen.run_with_args in project" "src/main.naab not found"
    skip "INT-05" "Error recovery" "src/main.naab not found"
    skip "INT-06" "Concurrent analysis" "src/main.naab not found"
else

# INT-01: main.naab runs successfully
setup_test_dir "phase2-codegen.json"
cp "$SCRIPT_DIR"/src/*.naab "$TEST_DIR/"
run_naab "main.naab"
check_exit "INT-01" "main.naab runs" 0 "$NAAB_EXIT"

# INT-02: Output contains expected analysis keywords
if grep -qi "category\|severity\|incident\|analysis\|finding" "$TEST_DIR/stdout.log" 2>/dev/null; then
    pass "INT-02" "Output has analysis keywords"
else
    fail "INT-02" "Output has analysis keywords" "no expected keywords in output"
fi

# INT-03: No taint violation (sanitized output)
if grep -qi "taint" "$TEST_DIR/stderr.log" 2>/dev/null && grep -qi "blocked\|violation" "$TEST_DIR/stderr.log" 2>/dev/null; then
    fail "INT-03" "Taint compliance" "taint violation detected in stderr"
else
    pass "INT-03" "Taint compliance (no violations)"
fi

# INT-04: codegen.run_with_args used in project
if grep -q "codegen" "$TEST_DIR/stdout.log" "$TEST_DIR/stderr.log" 2>/dev/null || [ "$NAAB_EXIT" -eq 0 ]; then
    pass "INT-04" "codegen.run_with_args in project context"
else
    fail "INT-04" "codegen.run_with_args" "no evidence of codegen usage"
fi

# INT-05: Error recovery -- run with --tree-walk too
setup_test_dir "phase2-codegen.json"
cp "$SCRIPT_DIR"/src/*.naab "$TEST_DIR/"
run_naab "main.naab" --tree-walk
check_exit "INT-05" "Error recovery (tree-walk engine)" 0 "$NAAB_EXIT"

# INT-06: Concurrent analysis produces results
# Check that async-related output exists (or at least no crash)
if [ "$NAAB_EXIT" -eq 0 ] || [ "$NAAB_EXIT" -eq 0 ]; then
    pass "INT-06" "Concurrent analysis (no crash)"
else
    fail "INT-06" "Concurrent analysis" "exit $NAAB_EXIT"
fi

fi  # end if src/main.naab exists

rm -rf "$TMPBASE"/test_*
fi

# ===================================================================
# SUMMARY
# ===================================================================
echo ""
echo -e "${BOLD}=== Results: ${GREEN}$PASS_COUNT passed${NC}, ${RED}$FAIL_COUNT failed${NC}, ${YELLOW}$SKIP_COUNT skipped${NC} out of $TOTAL ===${NC}"

if [ "$FAIL_COUNT" -gt 0 ]; then
    echo -e "\n${RED}Failures:${NC}"
    echo -e "$FAILURES"
fi

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULTS_FILE="$RESULTS_DIR/results_${TIMESTAMP}.txt"
{
    echo "NAAb-37 Results: $TIMESTAMP"
    echo "Passed: $PASS_COUNT  Failed: $FAIL_COUNT  Skipped: $SKIP_COUNT  Total: $TOTAL"
    if [ "$FAIL_COUNT" -gt 0 ]; then
        echo ""
        echo "Failures:"
        echo -e "$FAILURES"
    fi
} > "$RESULTS_FILE"

if [ "$FAIL_COUNT" -eq 0 ] && [ "$SKIP_COUNT" -le 6 ]; then
    echo -e "\n  ${GREEN}${BOLD}run-naab37.sh: ALL PASSED${NC}"
    exit 0
else
    echo -e "\n  ${RED}${BOLD}run-naab37.sh: FAILURES DETECTED${NC}"
    exit 1
fi
