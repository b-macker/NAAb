#!/bin/bash
# Governance App Test Runner
# Tests that app.naab PASSES and violation files FAIL as expected

NAAB="../../build/naab-lang"
PASS=0; FAIL=0; TOTAL=0

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m' # No Color

# Helper: expect success
expect_pass() {
    TOTAL=$((TOTAL + 1))
    output=$($NAAB "$1" 2>&1)
    if [ $? -eq 0 ]; then
        PASS=$((PASS + 1))
        echo -e "${GREEN}✓ PASS${NC}: $2"
    else
        FAIL=$((FAIL + 1))
        echo -e "${RED}✗ FAIL${NC}: $2 (expected pass, got error)"
        echo "  Output: $(echo "$output" | head -3)"
    fi
}

# Helper: expect governance error
expect_fail() {
    TOTAL=$((TOTAL + 1))
    output=$($NAAB "$1" 2>&1)
    exit_code=$?
    if [ $exit_code -ne 0 ] && (echo "$output" | grep -qE "Governance error|violation:"); then
        PASS=$((PASS + 1))
        echo -e "${GREEN}✓ PASS${NC}: $2 (correctly blocked)"
    else
        FAIL=$((FAIL + 1))
        echo -e "${RED}✗ FAIL${NC}: $2 (expected governance error)"
        echo "  Output: $(echo "$output" | head -3)"
    fi
}

echo "╔═══════════════════════════════════════════════════════╗"
echo "║       Governance App Test Suite                      ║"
echo "╚═══════════════════════════════════════════════════════╝"
echo

# Phase 1: Language Control
echo "═══ Phase 1: Language Control ═══"
expect_pass app.naab "Main app passes all checks"
expect_fail test_blocked_shell.naab "Shell language blocked"
echo

# Phase 2: Per-Language Rules
echo "═══ Phase 2: Per-Language Rules ═══"
expect_fail test_banned_eval.naab "eval() banned function blocked"
expect_fail test_blocked_subprocess.naab "subprocess import blocked"
echo

# Phase 3: Capabilities
echo "═══ Phase 3: Capabilities ═══"
expect_fail test_network_blocked.naab "Network access blocked in polyglot"
echo

# Phase 4: Resource Limits
echo "═══ Phase 4: Resource Limits ═══"
expect_fail test_too_many_blocks.naab "Too many polyglot blocks blocked"
expect_fail test_deep_recursion.naab "Deep recursion call depth blocked"
echo

# Phase 5: Code Quality (Security)
echo "═══ Phase 5: Code Quality (Security) ═══"
expect_fail test_secret.naab "Hardcoded API key detected"
expect_fail test_sql_injection.naab "SQL injection pattern blocked"
expect_fail test_path_traversal.naab "Path traversal blocked"
echo

# Phase 6: Code Quality (LLM Anti-Drift)
echo "═══ Phase 6: Code Quality (LLM Anti-Drift) ═══"
expect_fail test_oversimplified.naab "Oversimplified code blocked"
expect_fail test_incomplete.naab "Incomplete logic blocked"
expect_fail test_placeholder.naab "Placeholder marker blocked"
echo

# Phase 7: Polyglot Optimization
echo "═══ Phase 7: Polyglot Optimization ═══"
expect_fail test_polyglot_optimization.naab "Numerical computing in JS flagged (suggests Python)"
echo

echo "═══════════════════════════════════════════════════════"
if [ $FAIL -eq 0 ]; then
    echo -e "${GREEN}✓ ALL TESTS PASSED${NC}: $PASS/$TOTAL"
else
    echo -e "${RED}✗ SOME TESTS FAILED${NC}: $PASS/$TOTAL passed, $FAIL failed"
fi
echo "═══════════════════════════════════════════════════════"
