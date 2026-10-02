#!/usr/bin/env bash
# ============================================================
# test_escalation_message.sh — an escalated advisory says the run stops
#
# advisory_escalation turns the N-th occurrence of an advisory into an
# uncatchable block (exit 3). The message the operator reads was formatted at
# ADVISORY and ends "Note: This is an advisory warning — execution will
# continue", and the escalation only appended "This advisory was escalated
# after repeated occurrences." -- so the one explanation on screen said the run
# continued while the process was being terminated. Found in the repo-sentinel
# dogfood (round 6): two adversarial runs ended this way.
#
#   EM-01  the third occurrence still blocks (exit 3, code after it not run)
#   EM-02  ...and its message says execution stops
#   EM-03  control: below the escalation threshold the advisory does NOT say
#          execution stops and the run completes -- without it, EM-02 passes
#          for an engine that appends the stop line to every advisory
#
# Attribution. The occurrence count is keyed by RULE NAME for the whole engine,
# so every call site and every agent shares it. A kill message that gave only
# the count could not say whose occurrences ended the run.
#   EM-04  a static-rule escalation lists the three sites it counted
#   EM-05  context_drift.coherence_loss: two agents share ONE count -- alpha
#          dips twice, beta once, neither alone reaches soft_after=3, and the
#          run is killed with "<agent:alpha> x2, <agent:beta> x1"
#   EM-06  control: the same alpha sequence without beta completes -- so the
#          kill in EM-05 really was beta's occurrence added to alpha's, which
#          is the cross-agent sharing the message now makes visible
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"
_SYSTMP="${TMPDIR:-/tmp}"
TEST_TMP="${_SYSTMP}/escalation-message-$$"

PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo "  PASS [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo "  SKIP [$1] $2"; }

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
STUB_PID=""
cleanup() { [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null; teardown_isolated_trust; rm -rf "${TEST_TMP:?}"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"

# $1=dir $2=soft_after
mk_case() {
    mkdir -p "$1"
    cat > "$1/govern.json" <<EOF
{
    "mode": "enforce",
    "languages": { "allowed": ["javascript"] },
    "custom_rules": [{
        "id": "ESC-MSG", "name": "escalation_message", "pattern": "ESCALATE_TARGET",
        "level": "advisory", "enabled": true, "message": "Advisory escalation message test"
    }],
    "advisory_escalation": { "enabled": true, "soft_after": $2, "weight_multiplier": 2.0 },
    "security": { "sandbox_level": "elevated" }
}
EOF
    cat > "$1/test.naab" <<'EOF'
main {
    let a = <<javascript
// ESCALATE_TARGET
1
>>
    let b = <<javascript
// ESCALATE_TARGET
2
>>
    let c = <<javascript
// ESCALATE_TARGET
3
>>
    print("REACHED_END")
}
EOF
}

echo "=== an escalated advisory says the run stops ==="

# Viability: a javascript block must run at all here, or every verdict below
# is about the executor rather than the message.
V="$TEST_TMP/viable"; mkdir -p "$V"
echo '{ "mode": "enforce", "languages": { "allowed": ["javascript"] }, "security": { "sandbox_level": "elevated" } }' > "$V/govern.json"
printf 'main {\n    let x = <<javascript\n1\n>>\n    print("JS_OK")\n}\n' > "$V/test.naab"
VOUT="$(cd "$V" && timeout 60 "$NAAB" test.naab 2>&1)"
if [[ "$VOUT" != *JS_OK* ]]; then
    skip "EM-00" "javascript executor unusable here (UNMEASURABLE)"
    echo ""; echo "Results: $PASS_COUNT passed, $FAIL_COUNT failed, $SKIP_COUNT skipped"; exit 0
fi

mk_case "$TEST_TMP/esc" 3
OUT="$(cd "$TEST_TMP/esc" && timeout 60 "$NAAB" test.naab 2>&1)"; RC=$?
if [ "$RC" -eq 3 ] && [[ "$OUT" != *REACHED_END* ]] && [[ "$OUT" == *ESCALATED* ]]; then
    pass "EM-01" "third occurrence blocks (exit 3, code after it not run)"
else
    fail "EM-01" "escalation did not block (rc=$RC)" "$(tail -3 <<<"$OUT")"
fi
case "$OUT" in
    *"Execution stops here"*) pass "EM-02" "the escalated message says execution stops" ;;
    *) fail "EM-02" "escalated message still reads as if the run continues" \
            "$(grep -A2 -i 'escalated after' <<<"$OUT" | head -3)" ;;
esac

mk_case "$TEST_TMP/below" 5
OUT="$(cd "$TEST_TMP/below" && timeout 60 "$NAAB" test.naab 2>&1)"; RC=$?
if [ "$RC" -eq 0 ] && [[ "$OUT" == *REACHED_END* ]] && [[ "$OUT" != *"Execution stops here"* ]]; then
    pass "EM-03" "control: below the threshold the run completes and no stop line is printed"
else
    fail "EM-03" "below-threshold advisory changed behaviour (rc=$RC)" "$(tail -3 <<<"$OUT")"
fi

# ---- EM-04: static-rule attribution ----
OUT="$(cd "$TEST_TMP/esc" && timeout 60 "$NAAB" test.naab 2>&1)"
CNT="$(grep 'Occurrences counted:' <<<"$OUT" | head -1)"
NSITES=$(grep -o 'test\.naab:[0-9]* x1' <<<"$CNT" | sort -u | wc -l)
if [ "$NSITES" -eq 3 ]; then
    pass "EM-04" "static escalation lists the three sites it counted"
else
    fail "EM-04" "expected three distinct counted sites" "${CNT:-no attribution line}"
fi

# ---- EM-05 / EM-06: one count shared across agents ----
IS_WINDOWS=0
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) IS_WINDOWS=1 ;; esac
[ -n "${WINDIR:-}" ] && IS_WINDOWS=1
if [ "$IS_WINDOWS" -eq 1 ]; then
    skip "EM-05" "agent stub unsupported on Windows (UNMEASURABLE)"
    skip "EM-06" "agent stub unsupported on Windows (UNMEASURABLE)"
else
    source "$SCRIPT_DIR/../helpers/stub_launch.sh"
    export FAKE_KEY_ESC="fake-key-escalation-message"
    # $1=dir $2=include_beta(true|false). Sets ARC.
    run_agents() {
        local d="$1"; mkdir -p "$d"
        python3 -c '
import json, sys
r = {"content": "Reviewed the handler: input validation added, tests pass, no issues remain in this module.", "output_tokens": 30}
json.dump({"responses": [r] * 12}, open(sys.argv[1], "w"))
' "$d/fixture.json"
        STUB_PID=""; start_stub "$d/fixture.json" "$d" || return 1
        local ag='"provider":"gemini","model":"stub-model","api_base":"http://127.0.0.1:'"$STUB_PORT"'","api_key_env":"FAKE_KEY_ESC","system_prompt":"Review code for defects.","max_tokens":100,"max_turns":10,"retry":{"max_attempts":1,"backoff_ms":0}'
        cat > "$d/govern.json" <<EOF
{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" },
  "behavioral_sequences": { "enabled": true },
  "context_drift": { "enabled": true, "coherence_threshold": 0.9, "check_interval_turns": 1 },
  "advisory_escalation": { "enabled": true, "soft_after": 3 },
  "agents": { "alpha": { $ag }, "beta": { $ag } } }
EOF
        local beta_block=""
        if [ "$2" = "true" ]; then
            beta_block='    r = agent.send(b, "review the parser")
    agent.record_validation(b, false, "test_parser failed")
    r = agent.send(b, "review the parser again")'
        fi
        cat > "$d/t.naab" <<EOF
use agent
main {
    let a = agent.create("alpha")
    let b = agent.create("beta")
    let r = agent.send(a, "review the handler")
    agent.record_validation(a, false, "test_handler failed")
    r = agent.send(a, "review the handler again")
$beta_block
    r = agent.send(a, "review the handler once more")
    print("REACHED_END")
}
EOF
        (cd "$d" && timeout 60 "$NAAB" t.naab > out.txt 2>&1); ARC=$?
        kill "$STUB_PID" 2>/dev/null; STUB_PID=""
        return 0
    }

    if run_agents "$TEST_TMP/shared" true; then
        AOUT="$(cat "$TEST_TMP/shared/out.txt")"
        case "$AOUT" in
            *"Occurrences counted: <agent:alpha> x2, <agent:beta> x1"*)
                if [ "$ARC" -eq 3 ] && [[ "$AOUT" != *REACHED_END* ]]; then
                    pass "EM-05" "two agents share one count, and the kill names both (alpha x2, beta x1)"
                else
                    fail "EM-05" "attribution printed but the run was not blocked (rc=$ARC)"
                fi ;;
            *) fail "EM-05" "expected a kill attributed to alpha x2 and beta x1 (rc=$ARC)" \
                    "$(grep -E 'ESCALATED|Occurrences|WARNING context' <<<"$AOUT" | head -4)" ;;
        esac
    else
        skip "EM-05" "stub failed to start (UNMEASURABLE)"
    fi

    if run_agents "$TEST_TMP/alone" false; then
        AOUT="$(cat "$TEST_TMP/alone/out.txt")"
        if [ "$ARC" -eq 0 ] && [[ "$AOUT" == *REACHED_END* ]] && [[ "$AOUT" == *"coherence_loss (occurrence 2/3)"* ]]; then
            pass "EM-06" "control: alpha alone reaches 2 of 3 and completes, so EM-05's kill needed beta's occurrence"
        else
            fail "EM-06" "alpha alone did not stop at 2 of 3 (rc=$ARC)" \
                 "$(grep -E 'ESCALATED|WARNING context' <<<"$AOUT" | head -4)"
        fi
    else
        skip "EM-06" "stub failed to start (UNMEASURABLE)"
    fi
fi

echo ""
echo "Results: $PASS_COUNT passed, $FAIL_COUNT failed, $SKIP_COUNT skipped"
[ "$FAIL_COUNT" -eq 0 ]
