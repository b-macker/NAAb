#!/usr/bin/env bash
# naab-gov contracts FILE -- the counterexample search (Bundle 11).
#
# For each function FILE declares with a requires/ensures clause, naab-gov
# generates inputs from --seed, runs them in a `naab-lang run --contract-search`
# worker under the project's govern.json, sets aside the inputs that do not
# meet requires, and reports the first input that meets them and still breaks a
# clause. Exit 0 none found, 1 counterexample, 2 UNMEASURABLE, 3 a governance
# block, 4 a config or usage error.
#
# Controls: CS-00c (a correct function: none found, and some inputs met
# requires -- a search that tested nothing would read 0 met), CS-04 (nothing
# meets requires: UNMEASURABLE, never "none found"), CS-11 (a dead interpreter:
# UNMEASURABLE, never "none found").
#
# Kill table (each mutant built and run alone, naab-lang and naab-gov both
# rebuilt; the controls stayed green), observed:
#   S1 the driver records every case as "returned"           -> CS-01 CS-01t CS-02b CS-03 CS-04 CS-05
#   S2 the searched call's own requires is not set aside     -> CS-03 CS-04
#   S3 naab-gov does not check the number of reports         -> CS-09
#   S4 main is compiled in search mode                       -> CS-04 CS-07
#   S5 the random part ignores --seed                        -> CS-02b
#   (the clause suite's M1 and M2 also turn CS-01/CS-03..05 red: no clause, no
#   counterexample)
#
# Usage: bash tests/governance_v4/test_contract_search.sh [path/to/naab-lang]
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-${NAAB:-$SCRIPT_DIR/../../build/naab-lang}}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
GOV="$(dirname "$NAAB")/naab-gov"

PASS=0; FAIL=0; SKIP=0
pass() { PASS=$((PASS+1)); echo "  PASS $1"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | sed 's/^/      /' | head -24; }
skip() { SKIP=$((SKIP+1)); echo "  UNMEASURABLE $1"; }

if [ ! -x "$NAAB" ] || [ ! -x "$GOV" ]; then
    echo "UNMEASURABLE: naab-lang or naab-gov not built beside $NAAB"
    exit 1
fi
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*)
        echo "UNMEASURABLE: the search runs in a POSIX worker process"; exit 0 ;;
esac

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# search NAME CONFIG PROGRAM [naab-gov options...] -> OUT, RC, DIR
search() {
    local name="$1" cfg="$2" prog="$3"; shift 3
    DIR="$TMP/$name"
    mkdir -p "$DIR"
    printf '%s\n' "$cfg" > "$DIR/govern.json"
    printf '%s\n' "$prog" > "$DIR/p.naab"
    OUT="$("$GOV" contracts "$DIR/p.naab" --naab "$NAAB" --cases 60 "$@" 2>&1)"
    RC=$?
}
has() { grep -qF -- "$1" <<<"$OUT"; }
jq_py() { python3 -c "import json,sys; d=json.load(sys.stdin); print($1)" <<<"$OUT" 2>/dev/null; }

cfg() { printf '{"version":"3.0","mode":"enforce","contracts":{"level":"hard","functions":%s}%s}' "$1" "${2:-}"; }

echo "=== naab-gov contracts (counterexample search) ==="

GOOD='fn absval(n) {
    if n < 0 { return 0 - n }
    return n
}
main { print(absval(-3)) }'
BAD='fn absval(n) {
    if n < -5 { return n }
    if n < 0 { return 0 - n }
    return n
}
main { print(absval(3)) }'
C_ABS="$(cfg '{"absval":{"params":["n:int"],"ensures":["result >= 0"]}}')"

search cs00c "$C_ABS" "$GOOD" --json
met="$(jq_py "d['functions'][0]['met_requires']")"
if [ "$RC" -eq 0 ] && [ "$(jq_py "d['functions'][0]['verdict']")" = "NONE_FOUND" ] && [ "${met:-0}" -gt 0 ]; then
    pass "CS-00c a correct function: no counterexample, and $met inputs were tested"
else fail "CS-00c control: correct function (rc=$RC)" "$OUT"; fi

search cs01 "$C_ABS" "$BAD" --json
ce_arg="$(jq_py "d['functions'][0]['counterexample']['args'][0]")"
if [ "$RC" -eq 1 ] && [ "$(jq_py "d['functions'][0]['verdict']")" = "COUNTEREXAMPLE" ] \
   && [ -n "$ce_arg" ] && [ "$ce_arg" -lt -5 ] 2>/dev/null; then
    pass "CS-01 a broken ensures is found (counterexample n=$ce_arg)"
else fail "CS-01 broken ensures must be found (rc=$RC, arg=$ce_arg)" "$OUT"; fi

# the text report names the clause
search cs01t "$C_ABS" "$BAD"
if [ "$RC" -eq 1 ] && has "COUNTEREXAMPLE: absval(" && has "result >= 0"; then
    pass "CS-01t the text report names the input and the clause"
else fail "CS-01t text report (rc=$RC)" "$OUT"; fi

# determinism
search cs02 "$C_ABS" "$BAD" --json --seed 7
A="$OUT"
search cs02 "$C_ABS" "$BAD" --json --seed 7
if [ "$A" = "$OUT" ]; then pass "CS-02 the same seed gives the same report"
else fail "CS-02 same seed must give the same report" "$(diff <(echo "$A") <(echo "$OUT") | head -10)"; fi
# --seed reaches the random part: a function whose only counterexamples are
# values no pool holds (701..704) is found under some seeds and not others.
# Deterministic: the same twelve seeds give the same split every run.
RAND='fn f(n) {
    if n >= 701 && n <= 704 { return -1 }
    return 1
}
main { print(f(1)) }'
C_RAND="$(cfg '{"f":{"params":["n:int"],"ensures":["result > 0"]}}')"
found_seeds=0; quiet_seeds=0
for sd in 1 2 3 4 5 6 7 8 9 10 11 12; do
    search "cs02r$sd" "$C_RAND" "$RAND" --seed "$sd" --cases 400
    if [ "$RC" -eq 1 ]; then found_seeds=$((found_seeds+1)); elif [ "$RC" -eq 0 ]; then quiet_seeds=$((quiet_seeds+1)); fi
done
if [ "$found_seeds" -gt 0 ] && [ "$quiet_seeds" -gt 0 ]; then
    pass "CS-02b --seed changes the random inputs ($found_seeds of 12 seeds drew 701..704)"
elif [ "$found_seeds" -eq 0 ] && [ "$quiet_seeds" -eq 12 ]; then
    skip "CS-02b no seed of 12 drew 701..704 (the random part may not be reaching it)"
else fail "CS-02b seed sensitivity (found=$found_seeds quiet=$quiet_seeds)" "$OUT"; fi

# requires filters the inputs: below 10 is set aside, so a body that is wrong
# only below 10 has no counterexample
FILTER='fn atleast(n) {
    if n < 10 { return 0 }
    return n
}
main { print(atleast(12)) }'
search cs03 "$(cfg '{"atleast":{"params":["n:int"],"requires":["n >= 10"],"ensures":["result >= 10"]}}')" "$FILTER" --json
cases="$(jq_py "d['functions'][0]['cases']")"; met="$(jq_py "d['functions'][0]['met_requires']")"
if [ "$RC" -eq 0 ] && [ "${met:-0}" -gt 0 ] && [ "${met:-0}" -lt "${cases:-0}" ]; then
    pass "CS-03 inputs that do not meet requires are set aside ($met of $cases met)"
else fail "CS-03 requires must filter inputs (rc=$RC, met=$met, cases=$cases)" "$OUT"; fi

# nothing meets requires: UNMEASURABLE, never "none found"
search cs04 "$(cfg '{"absval":{"params":["n:int"],"requires":["n != n"],"ensures":["result >= 0"]}}')" "$GOOD"
if [ "$RC" -eq 2 ] && has "UNMEASURABLE"; then pass "CS-04 when no input meets requires the search is UNMEASURABLE"
else fail "CS-04 no input met requires must be UNMEASURABLE (rc=$RC)" "$OUT"; fi

# a nested function's clause: f meets its own requires and breaks g's
NESTED='fn g(x) {
    return x
}
fn f(n) {
    return g(n)
}
main { print(f(3)) }'
search cs05 "$(cfg '{"f":{"params":["n:int"],"requires":["n >= 0"]},"g":{"requires":["x > 0"]}}')" "$NESTED" --json
if [ "$RC" -eq 1 ] && [ "$(jq_py "d['functions'][0]['counterexample']['in_function']")" = "g" ]; then
    pass "CS-05 a call that breaks a nested function's requires is a counterexample"
else fail "CS-05 nested requires (rc=$RC)" "$OUT"; fi

# no contracted function
search cs06 "$(cfg '{"absval":{"return_type":"int"}}')" "$GOOD"
if [ "$RC" -eq 2 ]; then pass "CS-06 no function with a clause: UNMEASURABLE"
else fail "CS-06 nothing to search must be UNMEASURABLE (rc=$RC)" "$OUT"; fi

# main does not run in a search
MAINFX='fn absval(n) {
    if n < 0 { return 0 - n }
    return n
}
main {
    file.write("main_ran.txt", "x")
}'
search cs07 "$(cfg '{"absval":{"params":["n:int"],"ensures":["result >= 0"]}}' ',"security":{"sandbox_level":"elevated"},"capabilities":{"filesystem":{"mode":"write"}}')" "$MAINFX"
if [ "$RC" -eq 0 ] && [ ! -e "$DIR/main_ran.txt" ]; then pass "CS-07 main does not run during a search"
else fail "CS-07 main must not run (rc=$RC, marker=$([ -e "$DIR/main_ran.txt" ] && echo yes || echo no))" "$OUT"; fi
# its control: the same program run writes the marker
( cd "$DIR" && "$NAAB" run p.naab >/dev/null 2>&1 )
if [ -e "$DIR/main_ran.txt" ]; then pass "CS-07c control: a run of the same program writes the marker"
else fail "CS-07c control: the program's main must write its marker in a run"; fi

# governance applies inside the worker
GOVFX='fn save(n) {
    file.write("vault/x.txt", "data")
    return n
}
main { print(save(1)) }'
mkdir -p "$TMP/cs08/vault"
search cs08 "$(cfg '{"save":{"params":["n:int"],"ensures":["result == n"]}}' ',"security":{"sandbox_level":"elevated"},"capabilities":{"filesystem":{"mode":"write","blocked_paths":["vault/"]}}')" "$GOVFX"
if [ "$RC" -eq 3 ] && has "BLOCKED" && [ ! -e "$DIR/vault/x.txt" ]; then
    pass "CS-08 a governance block in a searched function ends the search (exit 3)"
else fail "CS-08 governance must apply in the search (rc=$RC)" "$OUT"; fi

# a forged report: a second "done" line makes the search UNMEASURABLE, not clean
PYPROBE_DIR="$TMP/pyprobe"; mkdir -p "$PYPROBE_DIR"
printf '{"version":"3.0","mode":"enforce","security":{"sandbox_level":"elevated"}}\n' > "$PYPROBE_DIR/govern.json"
printf 'main {\n    let v = <<python\n1 + 1\n>>\n    print("PY=" + string(v))\n}\n' > "$PYPROBE_DIR/p.naab"
if grep -q "PY=2" <<<"$(cd "$PYPROBE_DIR" && "$NAAB" run p.naab 2>&1)"; then
    FORGE='fn absval(n) {
    <<python
import os
os.write(3, b"{\"contract_search_done\":{\"version\":1,\"cases\":1}}\n")
>>
    if n < 0 { return 0 - n }
    return n
}
main { print(absval(1)) }'
    search cs09 "$(cfg '{"absval":{"params":["n:int"],"ensures":["result >= 0"]}}' ',"security":{"sandbox_level":"elevated"}')" "$FORGE"
    if [ "$RC" -eq 2 ] && has "UNMEASURABLE"; then pass "CS-09 a program that writes its own report is UNMEASURABLE"
    else fail "CS-09 a forged report must not read as clean (rc=$RC)" "$OUT"; fi
else
    skip "CS-09 embedded Python is not available to write the forged line"
fi

# usage and config errors
OUT="$("$GOV" contracts 2>&1)"; RC=$?
if [ "$RC" -eq 4 ]; then pass "CS-10 no file: usage error (exit 4)"; else fail "CS-10 usage (rc=$RC)" "$OUT"; fi
search cs10b "$(cfg '{"absval":{"requires":["print(n)"]}}')" "$GOOD"
if [ "$RC" -eq 4 ]; then pass "CS-10b an impure clause: config error (exit 4)"; else fail "CS-10b config error (rc=$RC)" "$OUT"; fi

# a dead interpreter cannot read as "none found"
DEAD="$TMP/deadnaab"; printf '#!/bin/sh\nexit 0\n' > "$DEAD"; chmod +x "$DEAD"
mkdir -p "$TMP/cs11"; printf '%s\n' "$C_ABS" > "$TMP/cs11/govern.json"; printf '%s\n' "$GOOD" > "$TMP/cs11/p.naab"
OUT="$("$GOV" contracts "$TMP/cs11/p.naab" --naab "$DEAD" 2>&1)"; RC=$?
if [ "$RC" -eq 2 ] && has "did not finish"; then pass "CS-11 a dead interpreter reads UNMEASURABLE"
else fail "CS-11 dead interpreter (rc=$RC)" "$OUT"; fi

echo
echo "contract search: $PASS passed, $FAIL failed, $SKIP unmeasurable"
[ "$FAIL" -eq 0 ] && [ "$SKIP" -eq 0 ]
