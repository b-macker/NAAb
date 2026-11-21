#!/usr/bin/env bash
# A184: a banned "Function(" must not refuse JavaScript's `function` keyword.
#
# languages.per_language.<lang>.banned_functions and
# restrictions.dangerous_calls.blocklist_extra match an entry as literal text,
# case-insensitively (naab/literal_entry_match.h, one copy for both). In
# JavaScript and TypeScript, which are case-sensitive, "Function(" -- the
# constructor -- therefore also matched every anonymous function,
# `function(x) {...}`, and a project that banned the constructor refused
# ordinary code (observed 2026-11-20 on c1312132). A91 fixed the same thing for
# restrictions.dangerous_calls' own patterns only. The shared matcher now skips
# a match whose leading word is spelled exactly `function` in those two
# languages and keeps searching; an entry spelled with the exact keyword is
# honoured, and every other language keeps case-insensitive matching.
#
# Arms (each refusal arm is a control that the fix did not widen the skip):
#   BK-00  CONTROL: no bans, the JavaScript block runs
#   BK-01  "Function(" banned: an anonymous function runs           (red on c1312132)
#   BK-02  CONTROL: a named function runs
#   BK-03  CONTROL: new Function("...") is refused, exit 3
#   BK-04  CONTROL: Function("...") without new is refused
#   BK-05  CONTROL: an anonymous function before the constructor: still refused
#   BK-06  CONTROL: an entry spelled "function(" refuses the keyword as written
#   BK-07  CONTROL: an alias of the constructor (FUNCTION = Function) is refused
#   BK-07b CONTROL: case is still ignored: a function named FUNCTION is refused
#   BK-08  CONTROL: Python keeps case-insensitive matching (EVAL( for "eval(")
#   BK-09  TypeScript: "Function(" banned, an anonymous function runs (red on c1312132;
#          UNMEASURABLE without tsx)
#   BX-01  blocklist_extra "Function(": an anonymous function runs  (red on c1312132)
#   BX-02  CONTROL: blocklist_extra still matches without case (MATH.MAX( for Math.max)
#
# Kill table (mutants of include/naab/literal_entry_match.h and its callers,
# run by hand, revert-checked 2026-11-20):
#   K1 reservedWordFor() returns "" for every language    -> BK-01, BK-09, BX-01
#   K2 a skipped keyword match ends the search (false)    -> BK-05
#   K3 the exact-entry exception removed (always skip)    -> BK-06
#   K4 the skip compares the leading word without case    -> BK-03, BK-04, BK-05, BK-07, BK-07b
#   K5 icase dropped for every language                   -> BK-07b, BK-08, BX-02
#      (BK-07 survives K5: the alias is refused without case-insensitive
#      matching, so BK-07b is the arm that pins case)
#   K6 checkBannedFunctions passes "" as the language      -> BK-01, BK-09
#   K7 blocklist_extra passes "" as the language           -> BX-01
#   K8 typescript left out of reservedWordFor()           -> BK-09
set -u
NAAB="${NAAB:-$(cd "$(dirname "$0")/../.." && pwd)/build/naab-lang}"
W="$(mktemp -d)"
source "$(dirname "$0")/../helpers/trust_setup.sh"
setup_isolated_trust
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT

PASS=0; FAIL=0; SKIP=0
ok()   { echo "  PASS [$1] $2"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -15 | sed 's/^/      | /'; FAIL=$((FAIL+1)); }
skip() { echo "  SKIP [$1] UNMEASURABLE: $2"; SKIP=$((SKIP+1)); }
if [ ! -x "$NAAB" ]; then echo "naab-lang not built: $NAAB"; exit 1; fi

# go ID EXTRA LANG CODE -> OUT, RC. EXTRA is appended to the config object.
go() {
    local d="$W/$1"; mkdir -p "$d"
    printf '{"version":"5.0","mode":"enforce","security":{"sandbox_level":"elevated"}%s}\n' "$2" > "$d/govern.json"
    printf 'main {\n    let r = <<%s\n%s\n>>\n    print("R=" + r)\n    print("DONE")\n}\n' "$3" "$4" > "$d/p.naab"
    OUT="$(cd "$d" && timeout 120 "$NAAB" run p.naab 2>&1)"; RC=$?
}
banned() { printf ',"languages":{"per_language":{"%s":{"banned_functions":%s}}}' "$1" "$2"; }
extra()  { printf ',"restrictions":{"dangerous_calls":{"level":"hard","blocklist_extra":%s}}' "$1"; }
has() { [[ "$OUT" == *"$1"* ]]; }
ran()     { [ "$RC" -eq 0 ] && has "R=$1" && has DONE; }
refused() { [ "$RC" -eq 3 ] && ! has DONE && has "$1"; }

ANON='[1, 2, 3].map(function(v) { return v * 2; }).join(",")'

echo "== banned_functions in JavaScript"
go BK00 "" javascript "$ANON"
if ran 2,4,6; then ok BK-00 "the block runs with no bans"; live=1
else bad BK-00 "the block does not run with no bans (rc=$RC)" "$OUT"; live=0; fi

go BK01 "$(banned javascript '["Function("]')" javascript "$ANON"
if [ "$live" -ne 1 ]; then skip BK-01 "BK-00 did not run"
elif ran 2,4,6; then ok BK-01 "\"Function(\" banned: an anonymous function runs"
else bad BK-01 "an anonymous function was refused (rc=$RC)" "$OUT"; fi

go BK02 "$(banned javascript '["Function("]')" javascript 'function add(x) { return x + 1; }
add(1)'
if ran 2; then ok BK-02 "a named function runs"; else bad BK-02 "a named function did not run (rc=$RC)" "$OUT"; fi

go BK03 "$(banned javascript '["Function("]')" javascript 'new Function("return 7")()'
if refused 'Banned function in javascript block: "Function("'; then ok BK-03 "new Function(...) is refused (exit 3)"
else bad BK-03 "new Function(...) was not refused (rc=$RC)" "$OUT"; fi

go BK04 "$(banned javascript '["Function("]')" javascript 'Function("return 7")()'
if refused 'Banned function in javascript block: "Function("'; then ok BK-04 "Function(...) is refused (exit 3)"
else bad BK-04 "Function(...) was not refused (rc=$RC)" "$OUT"; fi

go BK05 "$(banned javascript '["Function("]')" javascript "[1].map(function(v) { return v; });
new Function(\"return 7\")()"
if refused 'Banned function in javascript block: "Function("'; then ok BK-05 "the constructor after an anonymous function is still refused"
else bad BK-05 "the constructor after a skipped keyword was not refused (rc=$RC)" "$OUT"; fi

go BK06 "$(banned javascript '["function("]')" javascript "$ANON"
if refused 'Banned function in javascript block: "function("'; then ok BK-06 "an entry spelled \"function(\" refuses the keyword as written"
else bad BK-06 "an entry naming the keyword was not honoured (rc=$RC)" "$OUT"; fi

go BK07 "$(banned javascript '["Function("]')" javascript 'const FUNCTION = Function;
FUNCTION("return 7")()'
if refused 'Banned function in javascript block: "Function("'; then ok BK-07 "an alias of the constructor is refused"
else bad BK-07 "an alias of the constructor was not refused (rc=$RC)" "$OUT"; fi

go BK07b "$(banned javascript '["Function("]')" javascript 'function FUNCTION(s) { return s; }
FUNCTION("x")'
if refused 'Banned function in javascript block: "Function("'; then ok BK-07b "case is still ignored: FUNCTION( is refused"
else bad BK-07b "FUNCTION( was not refused for \"Function(\" (rc=$RC)" "$OUT"; fi

go BK08 "$(banned python '["eval("]')" python 'def EVAL(s):
    return s
EVAL("1")'
if refused 'Banned function in python block: "eval("'; then ok BK-08 "Python: EVAL( is refused for \"eval(\" (case ignored outside JavaScript)"
else bad BK-08 "Python lost case-insensitive matching (rc=$RC)" "$OUT"; fi

echo "== banned_functions in TypeScript"
go BK09c "" typescript "$ANON"
if ! ran 2,4,6; then
    skip BK-09 "a TypeScript block does not run here (rc=$RC): $(head -c 160 <<<"$OUT")"
else
    go BK09 "$(banned typescript '["Function("]')" typescript "$ANON"
    if ran 2,4,6; then ok BK-09 "TypeScript: \"Function(\" banned, an anonymous function runs"
    else bad BK-09 "TypeScript: an anonymous function was refused (rc=$RC)" "$OUT"; fi
fi

echo "== restrictions.dangerous_calls.blocklist_extra"
go BX01 "$(extra '["Function("]')" javascript "$ANON"
if [ "$live" -ne 1 ]; then skip BX-01 "BK-00 did not run"
elif ran 2,4,6; then ok BX-01 "blocklist_extra \"Function(\": an anonymous function runs"
else bad BX-01 "blocklist_extra refused an anonymous function (rc=$RC)" "$OUT"; fi

go BX02 "$(extra '["MATH.MAX("]')" javascript 'Math.max(1, 2)'
if refused 'MATH.MAX('; then ok BX-02 "blocklist_extra still matches without case"
else bad BX-02 "blocklist_extra did not refuse Math.max for MATH.MAX( (rc=$RC)" "$OUT"; fi

echo
echo "Results: $PASS passed, $FAIL failed, $SKIP unmeasurable"
[ "$FAIL" -eq 0 ]
