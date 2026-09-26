#!/usr/bin/env bash
# test_regex_timeout_bound.sh — a regex must stop at its time budget.
#
# SafeRegex (the ReDoS guard behind the `regex` stdlib module) had two defects
# that combined:
#
#   1. The nested-quantifier check was one regex, \([^)]*[*+?][^)]*\)[*+?{],
#      which a redundant pair of parentheses defeated: (a+)+b was rejected but
#      ((a+))+b -- the same catastrophic pattern -- passed validation.
#   2. The timeout could not fire. executeWithTimeout() threw after wait_for(),
#      but a std::async future's destructor blocks until the task finishes, so
#      the throw waited for the regex anyway. Measured on a 28-char input: 30s
#      against a 1s budget, and --timeout 5 fired at 30.5s -- it could not
#      preempt it either.
#
# Group A: the validator must reject the bypass, and must NOT reject ordinary
#          patterns (the old check also refused (?:ab)+).
# Group B: a pattern the validator allows but that is exponential must stop at
#          the budget. Asserted on WALL TIME: the error text alone was already
#          produced by the broken build, 29 seconds late.
# Group C: timed-out workers cannot be interrupted, only abandoned, so they are
#          capped; beyond the cap new work is refused rather than spawning
#          unbounded threads.
#
# B-01 is only meaningful if its pattern really is slow at this length.
# (a|a)+b quadruples every 2 characters (measured: n=20 takes 0.37s on raw
# std::regex), so n=30 needs minutes -- a result inside the bound can only come
# from the timeout. B-02 is the control that the same call on a short input
# still COMPLETES, so B-01 cannot pass by refusing everything.

set -uo pipefail
PASS=0
FAIL=0
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
NAAB="$REPO/build/naab-lang"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/naab_rxt.XXXXXX")"
[ -n "$WORK" ] && [ -d "$WORK" ] || { echo "FATAL: could not create work dir" >&2; exit 1; }
# The fixture is an unsigned govern.json; with any key in the trust store that
# is an integrity block, so the result must not depend on ~/.naab/trusted-keys.
source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust
trap 'rm -rf "$WORK"; teardown_isolated_trust' EXIT

if [ ! -x "$NAAB" ]; then
    echo "FAIL: naab-lang not built at $NAAB (UNMEASURABLE, not a pass)"
    exit 1
fi

echo '{ "version": "4.0", "mode": "off" }' > "$WORK/govern.json"

pass() { echo "  PASS [$1] $2"; PASS=$((PASS+1)); }
fail() { echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "         $3"; FAIL=$((FAIL+1)); }

A30="$(printf 'a%.0s' $(seq 30))"

# run_match ID PATTERN INPUT -> sets OUT (stdout+stderr) and ELAPSED (whole seconds)
run_match() {
    cat > "$WORK/$1.naab" <<EOF
use regex
main {
    try {
        print("RESULT:" + string(regex.matches("$3", "$2")))
    } catch (e) {
        print("CAUGHT:" + e["message"])
    }
}
EOF
    local start=$SECONDS
    OUT="$(cd "$WORK" && timeout 120 "$NAAB" "$WORK/$1.naab" 2>&1)"
    ELAPSED=$((SECONDS - start))
}

echo "=== A: validator ==="
run_match a01 '((a+))+b' "${A30}!"
case "$OUT" in
    *CAUGHT:*"dangerous regex pattern"*) pass A-01 "((a+))+b rejected by the validator (${ELAPSED}s)" ;;
    *) fail A-01 "((a+))+b was not rejected up front" "$(echo "$OUT" | head -2)" ;;
esac

run_match a02 '(a+)+b' "${A30}!"
case "$OUT" in
    *CAUGHT:*"dangerous regex pattern"*) pass A-02 "(a+)+b still rejected" ;;
    *) fail A-02 "(a+)+b no longer rejected" "$(echo "$OUT" | head -2)" ;;
esac

run_match a03 '(?:ab)+' "ababab"
case "$OUT" in
    *RESULT:true*) pass A-03 "(?:ab)+ allowed and matches (the old check refused it)" ;;
    *) fail A-03 "(?:ab)+ should be allowed" "$(echo "$OUT" | head -2)" ;;
esac

run_match a04 '[(]a+[)]+' "(aaa)"
case "$OUT" in
    *RESULT:true*) pass A-04 "parentheses inside a character class are literal" ;;
    *) fail A-04 "character-class parentheses misread as a group" "$(echo "$OUT" | head -2)" ;;
esac

echo "=== B: the budget holds ==="
run_match b01 '(a|a)+b' "${A30}!"
if [ "$ELAPSED" -le 10 ] && [[ "$OUT" == *CAUGHT:*"timed out"* ]]; then
    pass B-01 "exponential pattern stopped at the budget (${ELAPSED}s)"
else
    fail B-01 "not stopped within 10s (took ${ELAPSED}s)" "$(echo "$OUT" | head -2)"
fi

run_match b02 '(a|a)+b' "aaaab"
case "$OUT" in
    *RESULT:true*) pass B-02 "control: the same pattern on a short input completes" ;;
    *) fail B-02 "control failed -- B-01 may be refusing everything" "$(echo "$OUT" | head -2)" ;;
esac

echo "=== C: abandoned workers are capped ==="
cat > "$WORK/c01.naab" <<EOF
use regex
main {
    let i = 0
    while i < 6 {
        try {
            regex.matches("${A30}!", "(a|a)+b")
            print("RUN:done")
        } catch (e) {
            let m = e["message"]
            if m.contains("too many") {
                print("RUN:refused")
            } else {
                print("RUN:timeout")
            }
        }
        i = i + 1
    }
}
EOF
start=$SECONDS
OUT="$(cd "$WORK" && timeout 120 "$NAAB" "$WORK/c01.naab" 2>&1)"
ELAPSED=$((SECONDS - start))
timeouts=$(grep -c '^RUN:timeout' <<<"$OUT")
refused=$(grep -c '^RUN:refused' <<<"$OUT")
if [ "$timeouts" -ge 1 ] && [ "$refused" -ge 1 ] && [ "$ELAPSED" -le 30 ]; then
    pass C-01 "$timeouts timed out, then $refused refused at the cap (${ELAPSED}s)"
else
    fail C-01 "expected timeouts then refusals within 30s (timeouts=$timeouts refused=$refused ${ELAPSED}s)" "$(echo "$OUT" | head -3)"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
