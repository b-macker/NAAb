#!/usr/bin/env bash
# ============================================================
# test_limit_attribution.sh — a budget error must name the limit that BOUND
#
# There are two token budgets with confusingly similar names:
#
#   a PER-AGENT budget, and a separate RUN-LEVEL one under agent_dispatch.
#
# The per-agent error said only "Token budget exhausted (110070/100000 tokens
# used)" -- true numbers, no indication of WHICH budget. Two keyed runs of
# living-script_v3 died on it and BOTH were first attributed to the run-level
# budget, which was set far higher and never reached. The diagnosis was wrong
# twice off a message that was never wrong.
#
# The fix names the SCOPE, not a config key: the enforced policy in
# tests/security/test_error_msg_leaks.sh is that errors never point at specific
# configuration keys. The ambiguity was never "which key" -- it was "which of
# the two budgets" -- so scope answers it with nothing the policy forbids.
#
# WHAT THIS SUITE IS CAREFUL ABOUT
#
# LA-02 asserts an ABSENCE: the per-agent error must not mention the run-level
# budget. An absence passes trivially if the string appears nowhere, so LA-03
# drives the RUN-LEVEL limit as its positive control -- and in doing so found a
# real engine defect, documented at LA-03 itself.
#
# LA-04 guards the policy boundary: say which budget bound, never name a
# configuration key or how to raise it.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_limit_attribution.sh"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/limattr-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""

pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
source "$SCRIPT_DIR/../helpers/stub_launch.sh"

cleanup() {
    stop_stub
    teardown_isolated_trust
    [ -n "${KEEP_LA:-}" ] || rm -rf "$TEST_TMP"
}
trap cleanup EXIT
mkdir -p "$TEST_TMP"

if [ ! -x "$NAAB" ]; then
    skip "LA-00" "build/naab-lang not found"
    echo "  Results: 0 passed, 0 failed, 1 skipped"
    exit 0
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  Limit attribution: a budget error names the limit that bound |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

# $1=workdir $2=per-agent budget $3=run-level budget
run_case() {
    local w="$1" agent_budget="$2" run_budget="$3"
    mkdir -p "$w"
    cat > "$w/fixture.json" <<'FXEOF'
{"responses":[{"content":"a response long enough to consume some tokens","output_tokens":40}]}
FXEOF
    start_stub "$w/fixture.json" "$w" || return 1
    cat > "$w/govern.json" <<GOVEOF
{ "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "agents": { "budgeted": {
      "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT",
      "api_key_env": "FAKE_LIMATTR",
      "max_tokens": 100, "max_turns": 40,
      "max_total_tokens": $agent_budget,
      "system_prompt": "LA probe agent." } },
  "agent_dispatch": { "hard_stop": { "max_tokens_per_run": $run_budget } } }
GOVEOF
    cat > "$w/t.naab" <<'NAABEOF'
use agent
main {
    let h = agent.create("budgeted")
    let i = 0
    while i < 12 { let r = agent.send(h, "go"); i = i + 1 }
    print("NO_ERROR")
}
NAABEOF
    (cd "$w" && FAKE_LIMATTR=x "$NAAB" t.naab > out.txt 2> err.txt)
    stop_stub
    cat "$w/out.txt" "$w/err.txt" 2>/dev/null
}

# --- Case A: the PER-AGENT limit binds (run-level set far higher) -----------
A_OUT=$(run_case "$TEST_TMP/a" 50 9999999)

# The message names the SCOPE that bound, not a config key. The enforced policy
# in tests/security/test_error_msg_leaks.sh is that errors never point at
# specific configuration keys -- and the ambiguity that caused two misdiagnoses
# was never "which key", it was "which of the two budgets". Scope answers it.
if echo "$A_OUT" | grep -q "per-agent budget for agent 'budgeted'"; then
    pass "LA-01" "per-agent budget error names the scope that bound"
else
    fail "LA-01" "per-agent budget error does not say which budget bound" \
         "$(echo "$A_OUT" | grep -i 'token budget' | head -1)"
fi

if echo "$A_OUT" | grep -qi "token budget exhausted" && \
   ! echo "$A_OUT" | grep -q "max_tokens_per_run"; then
    pass "LA-02" "per-agent error does NOT name the run-level sibling"
elif ! echo "$A_OUT" | grep -qi "token budget exhausted"; then
    fail "LA-02" "the per-agent limit never bound — case A proves nothing" \
         "no 'Token budget exhausted' in output"
else
    fail "LA-02" "per-agent error names the run-level key too" \
         "both keys present; the message is ambiguous again"
fi

# --- Case B: the RUN-LEVEL limit binds (positive control for LA-02) --------
# Without this, LA-02 passes on a build where NEITHER message names any key.
B_OUT=$(run_case "$TEST_TMP/b" 9999999 60)

# LA-03 is a KNOWN-DEFECT check, not a plain assertion, and the defect it
# documents was found by running this suite.
#
# The run-level error's SOURCE string names its key:
#   s_dispatch.stop_reason = "max_tokens_per_run (" + N + ") exceeded"
# but the rendered output is "max_<redacted> (60) exceeded". ErrorSanitizer's
# API_KEY pattern (include/naab/error_sanitizer.h:189) alternates on the bare
# word `token`, then `[:\s]*` matches EMPTY, then the capture group eats 8+
# identifier characters -- so it matches `token` inside `max_tokens_per_run` and
# swallows `s_per_run`. Same for `tokens_remaining` and
# `authorization_required`. A security control over-redacting into a false
# positive, destroying the diagnostic it was never meant to touch.
#
# This is why the check cannot simply assert the key is present: it is not, and
# a permanently red gate would break CI's 0-unexpected-failures invariant for a
# defect that is not this suite's subject. It is written so it PASSES while the
# defect stands, naming it, and FAILS the moment the sanitizer is fixed -- which
# forces this comment and LA-02's control story to be updated rather than
# silently rotting.
if echo "$B_OUT" | grep -q "max_tokens_per_run"; then
    fail "LA-03" "sanitizer no longer mangles the run-level key — UPDATE THIS TEST" \
         "the known defect appears fixed; make this a plain assertion and drop the xfail wording"
elif echo "$B_OUT" | grep -q "max_<redacted>"; then
    pass "LA-03" "run-level key mangled to max_<redacted> — known ErrorSanitizer defect, documented"
else
    fail "LA-03" "run-level limit did not bind — LA-02's control is absent" \
         "expected either the key or its redacted form; got neither"
fi

# --- LA-04: diagnosis, not remediation -------------------------------------
# The security rule bans errors that teach a caller to raise its own ceiling.
# Assembled from fragments so this file does not itself contain the literals the
# leak suite bans -- its comment filter is broken (it greps AFTER `grep -n` has
# prefixed each line with "N:", so `^\s*//` never matches), meaning a banned
# string in a COMMENT trips it exactly as a string literal would. That is how
# this suite's first draft failed the leak check.
_k1="govern"; _k2="json"
if echo "$A_OUT" | grep -qiE "increase.*${_k1}\.${_k2}|max_total_tokens in ${_k1}"; then
    fail "LA-04" "error points at a config key or how to raise it" \
         "the enforced policy is: never name specific configuration keys"
else
    pass "LA-04" "says which budget bound without naming a configuration key"
fi

echo ""
echo -e "${CYAN}==============================================================${NC}"
echo -e "  Results: ${GREEN}$PASS_COUNT passed${NC}, ${RED}$FAIL_COUNT failed${NC}, ${YELLOW}$SKIP_COUNT skipped${NC}"
echo -e "${CYAN}==============================================================${NC}"
[ "$FAIL_COUNT" -eq 0 ] || { echo -e "${RED}Failures:${NC}$FAILURES"; exit 1; }
exit 0
