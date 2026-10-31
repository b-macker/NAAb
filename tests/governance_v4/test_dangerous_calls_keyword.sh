#!/usr/bin/env bash
# test_dangerous_calls_keyword.sh -- restrictions.dangerous_calls tells the
#                                    JavaScript keyword `function` from the
#                                    Function() constructor (A91)
#
# checkDangerousCall() matches every DANGEROUS_PATTERNS_DB entry without case,
# so `\bFunction\s*\(` -- the constructor -- also matched the reserved word in
# every anonymous function: `[1,2,3].map(function (v) {...})` was refused as
# "Function() constructor" (Matched: "function ("). #47 (915b43af) fixed the
# same false positive in checkCodeInjection() by making its API-name patterns
# case-sensitive; that fix never reached this copy.
#
# This copy keeps case-insensitive matching, because dropping it would let
# through PHP's case-insensitive function names (`SYSTEM(..)` runs) and an alias
# spelled in another case (`const FUNCTION = Function; FUNCTION(..)`), both
# refused today. (A Python `import os as OS` alias is not one of these: the
# engine resolves the alias before this check, so it reads `os.system(`.) Only a
# match that is EXACTLY the reserved word `function` is skipped: a reserved
# word cannot name a variable, so it is never the constructor or an alias of
# it. A keyword sweep of the table (javascript, python, ruby, shell, go, rust,
# cpp, csharp keyword lists; screened) found no other entry whose
# case-insensitive match hits a reserved word.
#
#   DK-00  CONTROL: an arrow function runs (the harness runs JavaScript and the
#          check passes code with no dangerous call)
#   DK-01  `function (v)` in a callback runs and returns its value
#   DK-01b `function(v)` with no space runs too
#   DK-02  Function("...")() is still refused (exit 3, the rule named)
#   DK-03  new Function("a", "...") is still refused
#   DK-04  an alias spelled FUNCTION(...) is still refused -- the arm a fully
#          case-sensitive fix would fail
#   DK-05  `function (v)` first, Function(...) later in the same block is
#          refused -- the arm a fix that stops at the first match would fail
#   DK-06  CONTROL: case-insensitive matching is kept for the rest of the
#          table: PHP's SYSTEM(...) is refused (Matched: "SYSTEM(")
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"
TEST_TMP=$(mktemp -d "${TMPDIR:-/tmp}/naab_dk_XXXXXX")
# The config is unsigned: with any key in the trust store it would be an
# integrity block, and every refusal arm would pass for it.
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
trap 'teardown_isolated_trust; rm -rf "$TEST_TMP"' EXIT

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | tail -8 | sed 's/^/       -> /'; }
unm()  { UNM=$((UNM+1));   echo "  UNMEASURABLE [$1] $2"; }

echo "=== dangerous_calls: the keyword function is not the Function() constructor ==="
if [ ! -x "$NAAB" ]; then echo "  naab-lang not built: UNMEASURABLE, not a pass"; exit 1; fi

cat > "$TEST_TMP/govern.json" <<'EOF'
{ "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "restrictions": { "dangerous_calls": "hard" } }
EOF

N=0
# $1 = language, $2 = block body; sets RC and OUT
run_block() {
    N=$((N+1))
    printf 'main {\n    let r = <<%s\n%s\n>>\n    print("R=" + r)\n}\n' "$1" "$2" > "$TEST_TMP/p$N.naab"
    OUT=$(cd "$TEST_TMP" && timeout 60 "$NAAB" "p$N.naab" </dev/null 2>&1); RC=$?
}
refused() { [ "$RC" -eq 3 ] && grep -qF 'Rule: restrictions.dangerous_calls' <<<"$OUT" && ! grep -q '^R=' <<<"$OUT"; }

# ---- DK-00 control -----------------------------------------------------------
run_block javascript '[1, 2, 3].map(v => v * 2).join(",")'
JS_OK=0
if [ "$RC" -eq 0 ] && grep -qx 'R=2,4,6' <<<"$OUT"; then
    JS_OK=1; pass DK-00 "CONTROL: an arrow function runs and returns 2,4,6"
elif grep -qi 'no executor\|not available' <<<"$OUT"; then
    unm DK-00 "no JavaScript executor here -- the JavaScript arms are UNMEASURABLE"
else fail DK-00 "the control block did not run cleanly (rc=$RC)" "$OUT"; fi

if [ "$JS_OK" -eq 1 ]; then
    run_block javascript '[1, 2, 3].map(function (v) { return v * 2; }).join(",")'
    if [ "$RC" -eq 0 ] && grep -qx 'R=2,4,6' <<<"$OUT"; then
        pass DK-01 "an anonymous function (with a space) runs"
    else fail DK-01 "an anonymous function was refused or failed (rc=$RC)" "$OUT"; fi

    run_block javascript '[1, 2, 3].map(function(v) { return v * 2; }).join(",")'
    if [ "$RC" -eq 0 ] && grep -qx 'R=2,4,6' <<<"$OUT"; then
        pass DK-01b "an anonymous function (no space) runs"
    else fail DK-01b "an anonymous function was refused or failed (rc=$RC)" "$OUT"; fi

    run_block javascript 'Function("return 6")()'
    if refused; then pass DK-02 "Function(\"...\")() is refused"
    else fail DK-02 "the constructor was not refused (rc=$RC)" "$OUT"; fi

    run_block javascript 'new Function("a", "return a * 2")(3)'
    if refused; then pass DK-03 "new Function(...) is refused"
    else fail DK-03 "new Function was not refused (rc=$RC)" "$OUT"; fi

    run_block javascript 'const FUNCTION = globalThis.Function; FUNCTION("return 6")()'
    if refused; then pass DK-04 "an alias spelled FUNCTION(...) is refused"
    else fail DK-04 "the upper-case alias was not refused (rc=$RC)" "$OUT"; fi

    run_block javascript '[1].map(function (v) { return v; }); Function("return 6")()'
    if refused; then pass DK-05 "Function(...) after an anonymous function in the same block is refused"
    else fail DK-05 "the constructor after a keyword match was not refused (rc=$RC)" "$OUT"; fi
else
    for a in DK-01 DK-01b DK-02 DK-03 DK-04 DK-05; do unm "$a" "no JavaScript executor here"; done
fi

# ---- DK-06 the rest of the table still matches without case --------------------
run_block php 'echo SYSTEM("true");'
if refused && grep -qF 'Matched: "SYSTEM("' <<<"$OUT"; then
    pass DK-06 "CONTROL: PHP SYSTEM(...) is refused (case-insensitive matching kept)"
elif [ "$RC" -ne 3 ] && grep -qi 'no executor\|not available' <<<"$OUT"; then
    unm DK-06 "no PHP executor here, and the block was not checked"
else fail DK-06 "PHP SYSTEM(...) was not refused (rc=$RC)" "$OUT"; fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed, $UNM unmeasurable ==="
[ "$FAIL" -eq 0 ]
