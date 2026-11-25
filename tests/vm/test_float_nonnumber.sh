#!/usr/bin/env bash
# test_float_nonnumber.sh -- VM arithmetic between a float and a non-number (A215).
#
# OP_ADD/SUB/MUL/DIV each had a float branch that needed only ONE double
# operand and read the other with asInt(). On a string, list, dict or null
# that reads the value's handle, not a number, so the VM printed a made-up
# result and exited 0 (observed on d32f8603): "a" * 2.5 -> 20, [1] - 1.5 ->
# 33.5, [1] + 1.5 -> 35.5, 2.5 / "a" -> 0.3125. The same operation with an int
# was already a type error ("a" - 2: "Cannot subtract int from string"), and
# the tree-walker refused the float cases too. Found by luck (A188), checking
# which string operations the run accepts. The branch is as old as the VM
# (1dd08e83). Both operands must now be numbers (bool already reads as an
# int there); every other pair reaches the existing type error.
#
#   FN-00      CONTROL: the harness runs a program and prints its value.
#   FN-01..16  each op x each non-number (string for - * /, list, dict, null),
#              with the float on either side: exit 1, a type error, no value.
#   FN-17      the error is catchable by try/catch, like the int path's.
#   FN-C1..C6  CONTROLS that keep their values: int+float, float*float,
#              bool*float (2.5), string+float concatenates, string*int repeats,
#              float+string concatenates.
#
# Kill table (mutants of src/vm/vm.cpp, run by hand):
#   V1 bothNumbers() returns true (the old branch)   -> FN-01..FN-16
#   V2 the guard dropped from OP_MUL only             -> FN-mul arms
#   V3 the guard dropped from OP_SUB only             -> FN-sub arms
#   V4 the guard dropped from OP_DIV only             -> FN-div arms
#   V5 the guard dropped from OP_ADD only             -> FN-add arms
#   V6 bothNumbers() rejects int (int+float refused)  -> FN-C1

set -uo pipefail
PASS=0; FAIL=0
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/naab_floatnn.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
ok()  { echo "  PASS $1: $2"; PASS=$((PASS+1)); }
bad() { echo "  FAIL $1: $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -5 | sed 's/^/      /'; FAIL=$((FAIL+1)); }
[ -x "$NAAB" ] || { echo "UNMEASURABLE: $NAAB not built"; exit 1; }

# runexpr EXPR -> OUT, RC. The operands come from variables, so constant
# folding (if any) cannot decide the arm.
runexpr() {
    printf 'main {\n    let s = "a"\n    let l = [1]\n    let d = {"k": 1}\n    let n = null\n    let f = 2.5\n    let r = %s\n    print("VALUE=" + string(r))\n}\n' "$1" > "$WORK/p.naab"
    OUT=$(cd "$WORK" && "$NAAB" run p.naab --no-governance 2>&1); RC=$?
}

runexpr 'f + 1'
if [ "$RC" -eq 0 ] && grep -q '^VALUE=3.5$' <<<"$OUT"; then ok FN-00 "CONTROL: the harness prints a computed value"
else bad FN-00 "the harness did not run (rc=$RC)" "$OUT"; echo "UNMEASURABLE"; exit 1; fi

refuse() {   # ID EXPR
    runexpr "$2"
    if [ "$RC" -ne 0 ] && grep -q 'Type error' <<<"$OUT" && ! grep -q '^VALUE=' <<<"$OUT"; then
        ok "$1" "$2 is a type error"
    else bad "$1" "$2 gave a value or another error (rc=$RC)" "$OUT"; fi
}
refuse FN-mul-01 's * f';  refuse FN-mul-02 'f * s'
refuse FN-mul-03 'l * f';  refuse FN-mul-04 'd * f';  refuse FN-mul-05 'n * f'
refuse FN-sub-06 's - f';  refuse FN-sub-07 'f - s'
refuse FN-sub-08 'l - f';  refuse FN-sub-09 'd - f';  refuse FN-sub-10 'n - f'
refuse FN-div-11 's / f';  refuse FN-div-12 'f / s'
refuse FN-div-13 'l / f';  refuse FN-div-14 'n / f'
refuse FN-add-15 'l + f';  refuse FN-add-16 'f + d';  refuse FN-add-16b 'n + f'

printf 'main {\n    let s = "a"\n    let r = "unset"\n    try {\n        r = s * 2.5\n    } catch (e) {\n        r = "CAUGHT"\n    }\n    print("R=" + r)\n}\n' > "$WORK/c.naab"
OUT=$(cd "$WORK" && "$NAAB" run c.naab --no-governance 2>&1); RC=$?
if [ "$RC" -eq 0 ] && grep -q '^R=CAUGHT$' <<<"$OUT"; then ok FN-17 "the type error is catchable, as the int path's is"
else bad FN-17 "not caught as an ordinary error (rc=$RC)" "$OUT"; fi

keep() {     # ID EXPR WANT
    runexpr "$2"
    if [ "$RC" -eq 0 ] && grep -qxF "VALUE=$3" <<<"$OUT"; then ok "$1" "CONTROL: $2 = $3"
    else bad "$1" "$2 should be $3 (rc=$RC)" "$OUT"; fi
}
keep FN-C1 '1 + f' '3.5'
keep FN-C2 'f * f' '6.25'
keep FN-C3 'true * f' '2.5'
keep FN-C4 's + f' 'a2.5'
keep FN-C5 's * 2' 'aa'
keep FN-C6 'f + s' '2.5a'

echo
echo "float with a non-number: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
