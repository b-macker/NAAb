#!/usr/bin/env bash
# test_line_breaks.sh -- a line break after '=' or after a key's ':' (A190).
#
# The parser skipped newlines after '=' in a plain assignment (x =\n 5, since
# 21d4b3d6) but not where a value follows '=' or ':' elsewhere: a let/const
# initialiser, a destructuring initialiser, a dict value, a struct literal's
# field value, a parameter's default (function and lambda), an enum's value.
# Each was a parse error ("Unexpected token in expression" at the newline).
# A line break there was never valid, so skipping it gives a meaning only to
# programs that did not parse before; it changes no program that ran.
#
# Each arm runs a program whose value follows its token on the NEXT line, on
# both engines, against the oracle: the same program written on one line, run
# by the same binary in the same arm (so the oracle is never typed by hand).
#   LB-00  CONTROL: the harness -- every one-line oracle runs and prints its
#          marker on both engines (a dead interpreter fails here).
#   LB-01..LB-09  a line break after the token: exit 0, same output as the oracle.
#   LB-10  CONTROL: plain assignment (already skipped) -- passes on the base too.
#   LB-11  CONTROL: an initialiser with no value at all still refuses to parse:
#          'let y =' followed by the end of the block, and a dict key with no
#          value ('{"a":' then '}').
#
# Kill table (mutants of src/parser/parser.cpp, run by hand from a copy):
#   P1 no skip after let/const '='                    -> LB-01, LB-02
#   P2 no skip after the dict ':'                      -> LB-03, LB-04
#   P3 no skip after the destructuring '='             -> LB-05
#   P4 no skip after the struct field ':'              -> LB-06
#   P5 no skip after a parameter default '=' (function
#      and lambda share the text, so one mutant)       -> LB-07, LB-08
#   P7 no skip after an enum value '='                 -> LB-09
# LB-10 and LB-11 are controls: they pass on the base build and after the fix.

set -uo pipefail
PASS=0; FAIL=0; UNMEAS=0
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/naab_linebreaks.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

ok()  { echo "  PASS $1: $2"; PASS=$((PASS+1)); }
bad() { echo "  FAIL $1: $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -8 | sed 's/^/      /'; FAIL=$((FAIL+1)); }

if [ ! -x "$NAAB" ]; then echo "UNMEASURABLE: $NAAB not built"; exit 1; fi

# run FILE ENGINE -> sets OUT, RC (no govern.json here; --no-governance waives
# the requirement, and the tree-walker is allowed without an enforce config).
run() {
    local tw=""; [ "$2" = tw ] && tw="--tree-walk"
    OUT=$(cd "$WORK" && "$NAAB" run "$1" --no-governance $tw 2>&1); RC=$?
    OUT=$(grep -v -E '^\s*To enforce governance|^\[governance\]|^Warning: no govern.json|^\s*$' <<<"$OUT")
}

# pair_arm ID DESC MARKER: $WORK/ID.one.naab is the oracle, ID.split.naab the
# same program with the line break; both engines.
pair_arm() {
    local id="$1" desc="$2" marker="$3" e o_out o_rc
    for e in vm tw; do
        run "$id.one.naab" "$e"; o_out="$OUT"; o_rc="$RC"
        if [ "$o_rc" -ne 0 ] || ! grep -qF -- "$marker" <<<"$o_out"; then
            bad "$id/$e" "the one-line oracle did not run (rc=$o_rc): UNMEASURABLE" "$o_out"
            UNMEAS=$((UNMEAS+1)); continue
        fi
        run "$id.split.naab" "$e"
        if [ "$RC" -eq 0 ] && [ "$OUT" = "$o_out" ]; then ok "$id/$e" "$desc"
        else bad "$id/$e" "$desc (rc=$RC)" "$OUT"; fi
    done
}

# case ID DESC PRELUDE ONE SPLIT MARKER
# PRELUDE is top-level text (struct/enum/fn); ONE and SPLIT are main's body.
case_arm() {
    local id="$1" desc="$2" prelude="$3" one="$4" split="$5" marker="$6"
    printf '%s\nmain {\n%s\n}\n' "$prelude" "$one"   > "$WORK/$id.one.naab"
    printf '%s\nmain {\n%s\n}\n' "$prelude" "$split" > "$WORK/$id.split.naab"
    pair_arm "$id" "$desc" "$marker"
}

echo "== LB-00 harness control"
printf 'main {\n    print("LB_ALIVE")\n}\n' > "$WORK/alive.naab"
live=1
for e in vm tw; do
    run alive.naab "$e"
    if [ "$RC" -eq 0 ] && grep -q '^LB_ALIVE$' <<<"$OUT"; then ok "LB-00/$e" "the interpreter runs a program and prints its output"
    else bad "LB-00/$e" "the interpreter did not run a trivial program (rc=$RC)" "$OUT"; live=0; fi
done
if [ "$live" -ne 1 ]; then echo "UNMEASURABLE: the harness control failed"; exit 1; fi

echo "== a line break after the token"
case_arm LB-01 "let: the value on the next line" "" \
  '    let y = 3
    print("Y=" + string(y))' \
  '    let y =
        3
    print("Y=" + string(y))' "Y=3"
case_arm LB-02 "const and a blank line before the value" "" \
  '    const z = 4
    print("Z=" + string(z))' \
  '    const z =

        4
    print("Z=" + string(z))' "Z=4"
case_arm LB-03 "dict: a quoted key's value on the next line" "" \
  '    let d = {"answer": 42, "b": 1}
    print(d)' \
  '    let d = {"answer":
        42, "b":
        1}
    print(d)' '"answer": 42'
case_arm LB-04 "dict: a bare key's value on the next line" "" \
  '    let d = {answer: 7}
    print(d)' \
  '    let d = {answer:
        7}
    print(d)' '"answer": 7'
case_arm LB-05 "destructuring: the value on the next line" "" \
  '    let [p, q] = [1, 2]
    print("PQ=" + string(p + q))' \
  '    let [p, q] =
        [1, 2]
    print("PQ=" + string(p + q))' "PQ=3"
case_arm LB-06 "struct literal: a field value on the next line" "struct Point { x: int, y: int }" \
  '    let pt = new Point { x: 1, y: 2 }
    print("PT=" + string(pt.x + pt.y))' \
  '    let pt = new Point { x:
        1, y:
        2 }
    print("PT=" + string(pt.x + pt.y))' "PT=3"
printf 'fn f(a = 5) { return a }\nmain {\n    print("F=" + string(f()))\n}\n' > "$WORK/LB-07.one.naab"
printf 'fn f(a =\n    5) { return a }\nmain {\n    print("F=" + string(f()))\n}\n' > "$WORK/LB-07.split.naab"
pair_arm LB-07 "function parameter default on the next line" "F=5"
case_arm LB-08 "lambda parameter default on the next line" "" \
  '    let g = fn(b = 6) { return b }
    print("G=" + string(g()))' \
  '    let g = fn(b =
        6) { return b }
    print("G=" + string(g()))' "G=6"
printf 'enum Color { Red = 10, Green }\nmain {\n    print("C=" + string(Color.Red))\n}\n' > "$WORK/LB-09.one.naab"
printf 'enum Color { Red =\n    10, Green }\nmain {\n    print("C=" + string(Color.Red))\n}\n' > "$WORK/LB-09.split.naab"
pair_arm LB-09 "enum value on the next line" "C="

echo "== controls"
case_arm LB-10 "CONTROL: plain assignment (skipped since 21d4b3d6)" "" \
  '    let x = 1
    x = 5
    print("X=" + string(x))' \
  '    let x = 1
    x =
        5
    print("X=" + string(x))' "X=5"
printf 'main {\n    let y =\n}\n' > "$WORK/LB-11a.naab"
printf 'main {\n    let d = {"a":\n    }\n    print(d)\n}\n' > "$WORK/LB-11b.naab"
for f in LB-11a LB-11b; do for e in vm tw; do
    run "$f.naab" "$e"
    if [ "$RC" -ne 0 ] && grep -qi 'parse\|unexpected\|expected' <<<"$OUT"; then ok "$f/$e" "CONTROL: a missing value is still a parse error"
    else bad "$f/$e" "a missing value parsed (rc=$RC)" "$OUT"; fi
done; done

echo
echo "line breaks: $PASS passed, $FAIL failed, $UNMEAS unmeasurable"
[ "$FAIL" -eq 0 ] && [ "$UNMEAS" -eq 0 ]
