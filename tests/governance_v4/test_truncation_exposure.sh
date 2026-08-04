#!/usr/bin/env bash
# ============================================================
# test_truncation_exposure.sh — truncation_count API surface
#
# AgentTracker.truncation_count was engine-internal only; scripts had to
# accumulate the per-response `truncated` boolean manually. It is now
# exposed in agent.usage() and agent.environment().state.
# Uses the local agent stub (finish_reason MAX_TOKENS -> truncated).
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

# EXPERIMENT (reversible in one commit). See test_absorption_degenerate.sh, which
# has carried this guard alone since before this change:
#   "Stub-backed HTTP tests hang on Windows/MSYS2 due to signal propagation and
#    process cleanup issues. Skip entirely — Linux CI validates the behavior."
# That diagnosis was made once and applied to one file out of 29 that launch the
# stub. build-windows has since stalled four times inside "CLI tests — shell
# suites": the step sits in_progress ~47 minutes, the runner is killed
# service-side, and the log archive 404s. timeout-minutes failed to fire TWICE,
# so the runner cannot enforce its own step timeout either — the cause cannot be
# read, only excluded.
#
# Read the next Windows run as the result:
#   green   -> stub-backed suites are the cause; narrow from here
#   stalls  -> they are exonerated and the cause is elsewhere in the phase
#
# Coverage is not lost: build-linux and Build & Test run every one of these in
# full, and agent-governance semantics are platform-neutral.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        echo "  test_truncation_exposure.sh: SKIPPED (stub-backed; excluded on Windows pending stall bisect)"
        exit 0 ;;
esac
if [ -n "${WINDIR:-}" ]; then
    echo "  test_truncation_exposure.sh: SKIPPED (stub-backed; excluded on Windows pending stall bisect)"
    exit 0
fi


if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/truncexp-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; TOTAL=0; FAILURES=""

pass() { PASS_COUNT=$((PASS_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
STUB_PID=""
cleanup() {
    [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null
    teardown_isolated_trust
    rm -rf "$TEST_TMP"
}
trap cleanup EXIT
mkdir -p "$TEST_TMP"

"$NAAB" --keygen "$TEST_TMP/test-key.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/test-key.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/test-key.pem"
export FAKE_KEY_TRUNC_TEST="fake-key-truncation-exposure-test"

sign_govern() {
    (cd "$1" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
}

start_stub() {  # $1=fixture $2=workdir
    STUB_PORT=$(( (RANDOM % 20000) + 20000 ))
    python3 "$SCRIPT_DIR/../helpers/agent_stub.py" "$STUB_PORT" "$1" "$2" > "$2/stub.log" 2>&1 &
    STUB_PID=$!
    for _ in $(seq 1 50); do
        grep -q READY "$2/stub.log" 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}
stop_stub() { [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null; wait "$STUB_PID" 2>/dev/null; STUB_PID=""; }

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|        truncation_count exposure (usage + environment)       |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

WDIR="$TEST_TMP/trunc"; mkdir -p "$WDIR"
cat > "$WDIR/fixture.json" << 'EOF'
{"responses": [
  {"content": "partial answer cut off mid", "finish_reason": "MAX_TOKENS", "output_tokens": 100},
  {"content": "another partial answer cut", "finish_reason": "MAX_TOKENS", "output_tokens": 100},
  {"content": "complete answer this time", "finish_reason": "STOP", "output_tokens": 20}
]}
EOF
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "T-00" "stub failed to start"; exit 1; }

cat > "$WDIR/govern.json" << GOVEOF
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "agents": {
        "test_agent": {
            "provider": "gemini",
            "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": "FAKE_KEY_TRUNC_TEST",
            "max_tokens": 100,
            "max_turns": 20
        }
    }
}
GOVEOF
sign_govern "$WDIR"

cat > "$WDIR/test.naab" << 'NAABEOF'
use agent

main {
    let h = agent.create("test_agent")
    let r1 = agent.send(h, "first question")
    let r2 = agent.send(h, "second question")
    let r3 = agent.send(h, "third question")
    print("R1_TRUNC=" + string(r1.get("truncated")))
    print("R3_TRUNC=" + string(r3.get("truncated")))
    let u = agent.usage(h)
    print("USAGE_TRUNC=" + string(u.get("truncation_count")))
    let env = agent.environment(h)
    let st = env.get("state")
    print("ENV_TRUNC=" + string(st.get("truncation_count")))
}
NAABEOF

OUTPUT=$(cd "$WDIR" && timeout 30s "$NAAB" test.naab 2>&1) || true
stop_stub

if echo "$OUTPUT" | grep -q "R1_TRUNC=true"; then
    pass "T-01" "Per-response truncated flag set for MAX_TOKENS (regression)"
else
    fail "T-01" "truncated flag missing on truncated response" "$(echo "$OUTPUT" | head -3)"
fi
if echo "$OUTPUT" | grep -q "R3_TRUNC=false"; then
    pass "T-02" "truncated flag false for STOP response (regression)"
else
    fail "T-02" "truncated flag wrong on complete response" "$(echo "$OUTPUT" | grep R3_TRUNC)"
fi
if echo "$OUTPUT" | grep -q "USAGE_TRUNC=2"; then
    pass "T-03" "agent.usage() exposes accumulated truncation_count"
else
    fail "T-03" "agent.usage() truncation_count wrong" "$(echo "$OUTPUT" | grep USAGE_TRUNC)"
fi
if echo "$OUTPUT" | grep -q "ENV_TRUNC=2"; then
    pass "T-04" "agent.environment() state exposes truncation_count"
else
    fail "T-04" "environment state truncation_count wrong" "$(echo "$OUTPUT" | grep ENV_TRUNC)"
fi

echo ""
echo "================================================"
echo "  PASS: $PASS_COUNT  FAIL: $FAIL_COUNT  SKIP: $SKIP_COUNT"
echo "================================================"
[ -n "$FAILURES" ] && echo -e "Failures:$FAILURES"

exit "$FAIL_COUNT"
