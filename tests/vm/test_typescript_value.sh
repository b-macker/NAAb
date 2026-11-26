#!/usr/bin/env bash
# test_typescript_value.sh -- a TypeScript block's value is not also printed (D18).
#
# The generic subprocess executor runs TypeScript through tsx and, to get a
# value back, wraps the block's last expression in console.log(). That line
# was both the value AND forwarded to the program's stdout, so
#   let r = <<typescript ... n >>  print("TS=" + string(r))
# printed "5TS=5" (observed 2026-10-29, and on d32f8603). A block that printed
# something itself got its own output inside the value too ("SIDE\n7").
# The executor now prints a per-call marker line before the value it prints
# (the expression is evaluated first, so anything it prints is the block's own
# output): what follows the marker is the value, what precedes it is the
# block's output and is forwarded as before. A block whose LAST line is its own
# console.log is not wrapped, gets no marker, and keeps its output as both
# value and printed text, as php, C++ and Go blocks do (TV-04, unchanged).
#
#   TV-00  CONTROL: tsx runs a block and the program prints its value.
#   TV-01  the value is printed once, by the program (not "5TS=5").
#   TV-02  the block's own console.log is printed, and the value is the last
#          expression (it was "SIDE\n7").
#   TV-03  output printed while the last expression is evaluated is the
#          block's output, not the value.
#   TV-04  CONTROL: a block whose last line is its own console.log: the value
#          and the printed text are that output (unchanged).
#   TV-05  a multi-line value (an object) after the marker is the whole value.
#
# Kill table (mutants of src/runtime/generic_subprocess_executor.cpp, run by hand):
#   G1 no marker printed (wrapper as before)         -> TV-01, TV-02, TV-05
#   G2 marker printed after evaluating, not before    -> TV-03
#   G3 marker set for every TypeScript block          -> TV-04 (value lost)
#   G4 stdout before the marker not forwarded         -> TV-02, TV-03
#
# Needs Linux and tsx on PATH (CI's Linux jobs install tsx 4.23.15); without
# it every arm is UNMEASURABLE.

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${NAAB:-${1:-$SCRIPT_DIR/../../build/naab-lang}}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"

echo "=== A TypeScript block's value is not also printed (D18) ==="
if [ "$(uname -s)" != "Linux" ] || ! command -v tsx >/dev/null 2>&1; then
    for a in TV-00 TV-01 TV-02 TV-03 TV-04 TV-05; do
        echo "  SKIP [$a] needs Linux and tsx on PATH -- UNMEASURABLE"
    done
    exit 0
fi

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust          # unsigned configs
W=$(mktemp -d "${TMPDIR:-/tmp}/naab_tsval_XXXXXX")
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT

PASS=0; FAIL=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -8 | sed 's/^/       /'; }

# run ID TS_BODY -> OUT (the program's stdout only, governance lines dropped), RC.
# 600 MB is the budget node 24's tsx needs (A145); node 22 fits it too.
run() {
    local d="$W/$1"; mkdir -p "$d"
    printf '{"mode":"enforce","security":{"sandbox_level":"elevated"},"runtime":{"memory_limit":1024}}\n' > "$d/govern.json"
    printf 'main {\n    print("BEFORE")\n    let r = <<typescript\n%s\n>>\n    print("TS=" + string(r))\n}\n' "$2" > "$d/p.naab"
    RC=0; OUT=$(cd "$d" && timeout 180s "$NAAB" run p.naab 2>/dev/null) || RC=$?
    OUT=$(grep -v -E '^\[governance\]|^-- Governance' <<<"$OUT" | tr -d '\r')
}

run TV-00 'const n: number = 2 + 3
n'
if [ "$RC" -eq 0 ] && grep -q 'TS=5' <<<"$OUT"; then pass TV-00 "CONTROL: tsx ran the block and the program printed its value"
else fail TV-00 "the block did not run (rc=$RC) -- every arm UNMEASURABLE" "$OUT"; echo "Results: $PASS passed, $FAIL failed"; exit 1; fi
want=$'BEFORE\nTS=5'
if [ "$OUT" = "$want" ]; then pass TV-01 "the value is printed once, by the program"
else fail TV-01 "stdout is not exactly BEFORE / TS=5" "$(printf '%s' "$OUT" | sed -n l)"; fi

run TV-02 'console.log("SIDE")
const n: number = 7
n'
if [ "$RC" -eq 0 ] && grep -q '^SIDE' <<<"$OUT" && grep -q 'TS=7$' <<<"$OUT" && ! grep -q 'TS=SIDE' <<<"$OUT"; then
    pass TV-02 "the block's own output is printed; the value is the last expression"
else fail TV-02 "expected SIDE printed and TS=7 (rc=$RC)" "$OUT"; fi

run TV-03 'function f(): number { console.log("IN"); return 9 }
f()'
if [ "$RC" -eq 0 ] && grep -q '^IN' <<<"$OUT" && grep -q 'TS=9$' <<<"$OUT" && ! grep -q 'TS=IN' <<<"$OUT"; then
    pass TV-03 "output printed while the value is computed is the block's, not the value"
else fail TV-03 "expected IN printed and TS=9 (rc=$RC)" "$OUT"; fi

run TV-04 'console.log(5)'
if [ "$RC" -eq 0 ] && grep -q 'TS=5$' <<<"$OUT"; then pass TV-04 "CONTROL: a block ending in its own console.log keeps that output as the value"
else fail TV-04 "expected TS=5 (rc=$RC)" "$OUT"; fi

run TV-05 'const o = {a: 1, b: [2, 3]}
o'
if [ "$RC" -eq 0 ] && grep -q 'TS={ a: 1, b: \[ 2, 3 \] }' <<<"$OUT" && [ "$(grep -o 'a: 1' <<<"$OUT" | wc -l)" -eq 1 ]; then
    pass TV-05 "a multi-line or object value is the whole value, printed once"
else fail TV-05 "expected one TS={ a: 1, b: [ 2, 3 ] } line (rc=$RC)" "$OUT"; fi

echo "Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
