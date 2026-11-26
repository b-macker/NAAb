#!/usr/bin/env bash
# test_range_loop.sh -- `for x in a..b` iterates without building a list (A66).
#
# The VM compiled `a..b` to OP_RANGE, which builds the whole list (and checks
# limits.array_size, since C10), and `for` then walked that list. So under
# `array_size: 1000` the VM refused `for i in 0..5000 { ... }` (exit 3) while
# the tree-walker, whose range is lazy, ran all 5000 iterations (observed on
# f7c3bcf1 and d32f8603). The owner's decision 17: a loop is iteration, not
# array growth. A plain `for x in a..b` now counts (OP_RANGE_ITER /
# OP_RANGE_NEXT); a range VALUE (`let xs = a..b`) still builds its list and
# stays capped (RL-03), and so does a loop with an index or destructuring,
# which keeps the list path.
#
#   RL-00  CONTROL: the harness runs a program.
#   RL-01  the counted loop yields exactly what the list path yields, for each
#          shape (exclusive, inclusive, empty, negative, break/continue,
#          nested, computed bounds, the top of the int range).
#   RL-02  under array_size 1000, `for i in 0..5000` runs all 5000 iterations.
#   RL-03  CONTROL: `let xs = 0..5000` is still refused under that limit.
#   RL-04  a tainted bound taints the loop variable: printing it is refused
#          (exit 3), as each element of the tainted list was.
#   RL-04c CONTROL: the same loop over clean bounds prints.
#   RL-05  a non-integer bound is the same type error as before.
#   RL-06  --timeout stops a long counted loop.
#
# Kill table (mutants of src/vm/compiler.cpp and src/vm/vm.cpp, run by hand):
#   R1 the counted path not taken (list path)   -> RL-02
#   R2 the loop variable gets no taint          -> RL-04
#   R3 inclusive treated as exclusive           -> RL-01
#   R4 no null bound after INT32_MAX (wraps)    -> RL-01 (top of the range)
#   R5 bounds not type-checked                  -> RL-05

set -uo pipefail
PASS=0; FAIL=0
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
W="$(mktemp -d "${TMPDIR:-/tmp}/naab_rangeloop.XXXXXX")"
source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT
ok()  { echo "  PASS $1: $2"; PASS=$((PASS+1)); }
bad() { echo "  FAIL $1: $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -6 | sed 's/^/      /'; FAIL=$((FAIL+1)); }
[ -x "$NAAB" ] || { echo "UNMEASURABLE: $NAAB not built"; exit 1; }

nr() { OUT=$(cd "$W" && timeout 60 "$NAAB" run "$1" --no-governance "${@:2}" 2>&1); RC=$?; }

printf 'main {\n    print("ALIVE")\n}\n' > "$W/alive.naab"; nr alive.naab
if [ "$RC" -eq 0 ] && grep -q '^ALIVE$' <<<"$OUT"; then ok RL-00 "CONTROL: the harness runs a program"
else bad RL-00 "the harness did not run (rc=$RC)" "$OUT"; exit 1; fi

# RL-01: each shape twice -- a direct range loop (counted) and the same range
# bound to a variable first (a value: the list path, the oracle).
shape=0
for spec in '0..5' '2..=4' '5..2' '-3..0' '0..=0' '2147483645..=2147483647' 'n..n+2'; do
    shape=$((shape+1))
    body='if i == 1 { continue }
        if i == 2147483646 { continue }
        out.push(i)
        if i == 40 { break }'
    printf 'main {\n    let n = 38\n    let out = []\n    for i in %s {\n        %s\n    }\n    print("V=" + string(out))\n}\n' "$spec" "$body" > "$W/d$shape.naab"
    printf 'main {\n    let n = 38\n    let out = []\n    let xs = %s\n    for i in xs {\n        %s\n    }\n    print("V=" + string(out))\n}\n' "$spec" "$body" > "$W/o$shape.naab"
    nr "o$shape.naab"; o="$OUT"; orc=$RC
    nr "d$shape.naab"
    if [ "$orc" -eq 0 ] && grep -q '^V=' <<<"$o" && [ "$RC" -eq 0 ] && [ "$(grep '^V=' <<<"$OUT")" = "$(grep '^V=' <<<"$o")" ]; then
        ok "RL-01.$shape" "for i in $spec yields what the list path yields: $(grep '^V=' <<<"$OUT")"
    else bad "RL-01.$shape" "for i in $spec (rc=$RC, oracle rc=$orc)" "counted: $OUT
list: $o"; fi
done
printf 'main {\n    let f = 0\n    for i in 0..3 {\n        for j in 0..=i { f = f + j }\n    }\n    print("V=" + string(f))\n}\n' > "$W/nest.naab"; nr nest.naab
if [ "$RC" -eq 0 ] && grep -q '^V=4$' <<<"$OUT"; then ok RL-01.n "nested counted loops"; else bad RL-01.n "nested (rc=$RC)" "$OUT"; fi

mkdir -p "$W/lim"
printf '{"version":"3.0","mode":"enforce","limits":{"array_size":1000}}\n' > "$W/lim/govern.json"
printf 'main {\n    let c = 0\n    for i in 0..5000 { c = c + 1 }\n    print("C=" + string(c))\n}\n' > "$W/lim/loop.naab"
printf 'main {\n    let xs = 0..5000\n    print("LEN=" + string(xs.length()))\n}\n' > "$W/lim/value.naab"
OUT=$(cd "$W/lim" && "$NAAB" run loop.naab 2>&1); RC=$?
if [ "$RC" -eq 0 ] && grep -q '^C=5000$' <<<"$OUT"; then ok RL-02 "a loop over 0..5000 runs under array_size 1000 (no list is built)"
else bad RL-02 "the loop was refused or short (rc=$RC)" "$OUT"; fi
OUT=$(cd "$W/lim" && "$NAAB" run value.naab 2>&1); RC=$?
if [ "$RC" -eq 3 ] && ! grep -q '^LEN=' <<<"$OUT"; then ok RL-03 "CONTROL: the range VALUE 0..5000 is still refused under the limit"
else bad RL-03 "the range value was not refused (rc=$RC)" "$OUT"; fi

mkdir -p "$W/taint"
printf '{"version":"3.0","mode":"enforce","security":{"sandbox_level":"elevated"},"taint_tracking":{"enabled":true,"level":"hard","sources":["env.get"],"sinks":["print"]}}\n' > "$W/taint/govern.json"
printf 'use env\nmain {\n    let n = int(env.get("RL_N"))\n    for i in 0..n {\n        print(i)\n    }\n}\n' > "$W/taint/t.naab"
printf 'main {\n    let n = 2\n    for i in 0..n {\n        print("I=" + string(i))\n    }\n}\n' > "$W/taint/c.naab"
OUT=$(cd "$W/taint" && RL_N=2 "$NAAB" run t.naab 2>&1); RC=$?
if [ "$RC" -eq 3 ] && grep -qi 'taint' <<<"$OUT"; then ok RL-04 "a tainted bound taints the loop variable (print refused)"
else bad RL-04 "the tainted loop variable reached print (rc=$RC)" "$OUT"; fi
OUT=$(cd "$W/taint" && "$NAAB" run c.naab 2>&1); RC=$?
if [ "$RC" -eq 0 ] && grep -q '^I=1$' <<<"$OUT"; then ok RL-04c "CONTROL: the same loop over clean bounds prints"
else bad RL-04c "the clean loop did not print (rc=$RC)" "$OUT"; fi

printf 'main {\n    for i in 0..2.5 { print(i) }\n}\n' > "$W/bad.naab"; nr bad.naab
if [ "$RC" -ne 0 ] && grep -q 'Range bounds must be integers' <<<"$OUT"; then ok RL-05 "a non-integer bound is a type error"
else bad RL-05 "expected 'Range bounds must be integers' (rc=$RC)" "$OUT"; fi

printf 'main {\n    let c = 0\n    for i in 0..2000000000 { c = c + 1 }\n    print("DONE")\n}\n' > "$W/long.naab"
start=$(date +%s); nr long.naab --timeout 1; took=$(( $(date +%s) - start ))
if [ "$RC" -ne 0 ] && ! grep -q '^DONE$' <<<"$OUT" && [ "$took" -lt 30 ]; then ok RL-06 "--timeout stops a long counted loop (${took}s)"
else bad RL-06 "the loop was not stopped (rc=$RC, ${took}s)" "$OUT"; fi

echo
echo "range loops: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
