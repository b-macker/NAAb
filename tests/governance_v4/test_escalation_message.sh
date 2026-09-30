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
cleanup() { teardown_isolated_trust; rm -rf "${TEST_TMP:?}"; }
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

echo ""
echo "Results: $PASS_COUNT passed, $FAIL_COUNT failed, $SKIP_COUNT skipped"
[ "$FAIL_COUNT" -eq 0 ]
