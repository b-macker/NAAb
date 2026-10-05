#!/usr/bin/env bash
# ============================================================
# test_comment_marker_context.sh -- a line-comment marker is a comment only
# where the language's real lexer says so
#
# #294 put the comment-stripping facts in the language table but wrote one too
# simply: a line-comment marker matched ANYWHERE. `#` then fired inside shell
# ${#a[@]} / $# / a#b, PHP #[ (an attribute), and Ruby ?# / /#/ -- none of
# which start a comment. A stray `#` treated as a comment runs to end of line,
# eating a real string's opening quote; the code after that string is then
# stripped as if it were string contents and never reaches the injection check.
#
# Each case has two parts:
#   REAL  the construct is valid code in the language's OWN interpreter -- the
#         `#` is genuinely not a comment (skipped when the interpreter is
#         absent). This is the external oracle: the engine must agree with it.
#   SEEN  a dangerous call placed AFTER <mid-token #> + a quoted string is
#         still detected (exit 3). Only correct (non-comment) handling of the
#         `#` keeps that call visible; revert the table's context rule and the
#         call is hidden and runs, so this assertion fails -- it is not vacuous
#         (verified against a build with the rule removed).
# A control (CT) checks a genuine line-start comment is STILL a comment, so the
# fix did not simply stop treating `#` as a comment.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="$REPO/build/naab-lang"

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | tail -6 | sed 's/^/       | /'; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP [$1] $2"; }

echo "=== line-comment marker context ==="
if [ ! -x "$NAAB" ] && [ ! -x "$NAAB.exe" ]; then
    skip CM-00 "naab-lang not built -- UNMEASURABLE"
    echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"; exit 0
fi

W="$(mktemp -d)"
trap 'rm -rf "$W"; teardown_isolated_trust' EXIT
printf '%s\n' '{"mode":"enforce","security":{"sandbox_level":"elevated"},"restrictions":{"shell_injection":{"enabled":true,"level":"hard"},"code_injection":{"enabled":true,"level":"hard"}}}' > "$W/govern.json"
SQ="'"

# seen ID LANG BLOCK : the block holds a dangerous call reachable only if the
# mid-token # is NOT read as a comment. Expect exit 3, program not run.
seen() {
    local id="$1" lang="$2" block="$3"
    printf 'main {\n    <<%s\n%s\n>>\n    print("RAN")\n}\n' "$lang" "$block" > "$W/p.naab"
    local out rc; out="$(cd "$W" && "$NAAB" p.naab --timeout 30 2>&1)"; rc=$?
    if [ $rc -eq 3 ] && [[ "$out" != *RAN* ]]; then ok "$id" "dangerous call after mid-token # stays visible (blocked)"
    else bad "$id" "code after a mid-token # was hidden (exit $rc)" "$out"; fi
}

# real ID CMD... : the construct parses as valid code in the real interpreter.
real() {
    local id="$1"; shift
    if ! command -v "$1" >/dev/null 2>&1; then skip "$id" "$1 not installed -- UNMEASURABLE"; return; fi
    if "$@" >/dev/null 2>&1; then ok "$id" "the construct is valid code in the real interpreter"
    else bad "$id" "the construct did not parse as code -- fixture wrong"; fi
}

# --- shell: ${#arr[@]}, $#, a#b ---
real CM-sh-real bash -n <(printf 'arr=(a b); n=${#arr[@]}; echo "$n"\n')
seen CM-sh-arrlen shell "$(printf 'arr=(a b); n=${#arr[@]}; msg=%s\n%s; eval $(echo id)' "$SQ" "$SQ")"
seen CM-sh-argc   shell "$(printf 'set -- a b; n=$#; msg=%s\n%s; eval $(echo id)' "$SQ" "$SQ")"
seen CM-sh-word   shell "$(printf 'x=a#b; msg=%s\n%s; eval $(echo id)' "$SQ" "$SQ")"

# --- ruby: ?#, /#/ ---
real CM-rb-real-q  ruby -c <(printf 'c = ?#; p c\n')
real CM-rb-real-re ruby -c <(printf 'x = /#/; p x\n')
seen CM-rb-charlit ruby "$(printf 'c = ?#; s = %s\n%s; eval(x)' "$SQ" "$SQ")"
seen CM-rb-regex   ruby "$(printf 'r = /#/; s = %s\n%s; eval(x)' "$SQ" "$SQ")"

# --- php: #[attribute] is not a comment (a correctness fact, not a desync:
# the stripper passes comment text through, and a php attribute cannot carry
# code after it on one line -- so there is no sharp SEEN arm, only the oracle).
real CM-php-real php -l <(printf '<?php\n#[Deprecated]\nfunction f() {}\n')

# --- CT: a genuine line-start comment is STILL a comment ---
# An apostrophe in a real # comment must not open a string that hides the next
# line (the bug #294's comment handling was meant to fix). The eval on its own
# line stays visible and blocks; the apostrophe line does not swallow it.
printf 'main {\n    <<shell\n# it%ss fine\neval $(echo id)\n>>\n    print("RAN")\n}\n' "$SQ" > "$W/p.naab"
cto="$(cd "$W" && "$NAAB" p.naab --timeout 30 2>&1)"; ctrc=$?
if [ $ctrc -eq 3 ] && [[ "$cto" != *RAN* ]]; then ok CM-CT "an apostrophe in a real # comment still does not hide the next line"
else bad CM-CT "a real # comment mis-handled (exit $ctrc)" "$cto"; fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
