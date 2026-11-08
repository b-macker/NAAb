#!/usr/bin/env bash
# test_async_array_callbacks.sh -- the array module's callbacks inside a VM async fn (A124)
#
# THE DEFECT
#
# A VM async fn runs on a fresh VM with its own StdLib (V-CONC-007), and only
# VM::execute() and importModule() pointed the array module at a VM to call
# callbacks through (its "evaluator"). The async worker calls
# callNaabFunction() directly and never ran execute(), so inside an async fn
# array.map_fn / filter_fn / reduce_fn / find stopped the call with "Internal
# error: ... requires function evaluator", and array.sort / sorted with a
# comparator returned the DEFAULT order -- the comparator was dropped without a
# word (observed on master 977e7a40: [1, 2] where main{} gets [2, 1]). The
# worker now wires the evaluator to its own VM (VM::wireArrayEvaluator(), the
# one helper execute() and importModule() share), and sort/sorted refuse a
# comparator they cannot call instead of ignoring it (unit tests
# ArrayModuleTest.SortRefusesComparatorWithoutEvaluator and its two controls).
# With the evaluator, A120(a)'s callback-argument taint reaches callbacks inside
# an async fn too.
#
#   AC-00   CONTROL: the six calls in main{} give the expected values
#   AC-01   map_fn inside an async fn
#   AC-02   filter_fn inside an async fn
#   AC-03   reduce_fn inside an async fn
#   AC-04   find inside an async fn
#   AC-05   sort with a comparator inside an async fn (descending)
#   AC-06   sorted with a comparator inside an async fn (descending)
#   AC-07   a callback's throw inside an async fn reaches the await's catch as
#           the thrown value
#   AC-08   a callback inside an async fn given tainted data: its print(v) is
#           flagged at the callback's line (A120(a) through the async VM)
#   AC-08c  CONTROL: the same callback given clean data in the same run is not
#           flagged -- AC-08 is the taint, not every callback
#   AC-09   CONTROL: the same tainted callback in main{} is flagged (the
#           detector sees this shape at all)
#
# Kill table (each mutant rebuilt, suite re-run):
#   the worker does not call wireArrayEvaluator()     -> AC-01..AC-08 red
#   the evaluator passes no callback taint (false)    -> AC-08, AC-09 red
# Red first (observed): on master 977e7a40, AC-01..AC-08c fail (map_fn and the
# others stop the async fn, so AC-08's run never finishes; sort and sorted
# return [1, 2]; the callback's throw is replaced by the evaluator error); the
# controls AC-00 and AC-09 pass.

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust          # unsigned configs
W=$(mktemp -d "${TMPDIR:-/tmp}/naab_asyncarr_XXXXXX")
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -8 | sed 's/^/       -> /'; }

if [ ! -x "$NAAB" ] && [ ! -x "$NAAB.exe" ]; then echo "UNMEASURABLE: naab-lang not built"; exit 1; fi

BASE='"mode": "enforce", "security": {"sandbox_level": "elevated"}'
TAINT='"taint_tracking": {"enabled": true, "level": "advisory", "sources": ["env.get"], "sinks": ["print"], "sanitizers": ["sanitize_"]}'

# run ID CONFIG PROGRAM -> OUT, RC
run() {
    local d="$W/$1"; mkdir -p "$d"
    printf '%s\n' "$2" > "$d/govern.json"
    printf '%s' "$3" > "$d/p.naab"
    RC=0; OUT=$(cd "$d" && timeout 60s "$NAAB" run p.naab 2>&1) || RC=$?
}
has() { grep -q -F -- "$1" <<<"$OUT"; }
# The lines of p.naab a taint violation names.
flagged_lines() { grep -oE 'Taint tracking violation.*p\.naab:[0-9]+' <<<"$OUT" | grep -oE '[0-9]+$' | sort -u | tr '\n' ' ' | sed 's/ $//'; }

echo "=== array callbacks inside a VM async fn (A124) ==="

CALLS='    let m = array.map_fn([1, 2], fn(v) { return v + 1 })
    print("MAP=" + string(m))
    let f = array.filter_fn([1, 2, 3], fn(v) { return v > 1 })
    print("FILTER=" + string(f))
    let r = array.reduce_fn([1, 2, 3], fn(a, v) { return a + v }, 0)
    print("REDUCE=" + string(r))
    let d = array.find([1, 2, 3], fn(v) { return v == 2 })
    print("FIND=" + string(d))
    let s = array.sort([1, 2], fn(a, b) { return b - a })
    print("SORT=" + string(s))
    let s2 = array.sorted([1, 2], fn(a, b) { return b - a })
    print("SORTED=" + string(s2))'
EXPECT=("MAP=[2, 3]" "FILTER=[2, 3]" "REDUCE=6" "FIND=2" "SORT=[2, 1]" "SORTED=[2, 1]")
NAMES=(map_fn filter_fn reduce_fn find sort sorted)

run ac00 "{ $BASE }" "use array
main {
$CALLS
    print(\"DONE\")
}
"
miss=""
for e in "${EXPECT[@]}"; do has "$e" || miss="$miss [$e]"; done
if [ "$RC" -eq 0 ] && has "DONE" && [ -z "$miss" ]; then
    pass AC-00 "CONTROL: the six calls in main{} give the expected values"
else
    fail AC-00 "main{} did not give the expected values (rc=$RC, missing:$miss)" "$OUT"
fi

run ac01 "{ $BASE }" "use array
async fn work() {
$CALLS
    return 1
}
main {
    let r = await work()
    print(\"DONE\")
}
"
for i in 0 1 2 3 4 5; do
    id="AC-0$((i+1))"
    if has "${EXPECT[$i]}"; then
        pass "$id" "${NAMES[$i]} inside an async fn gives ${EXPECT[$i]#*=}"
    else
        got=$(grep -m1 -E "^${EXPECT[$i]%%=*}=" <<<"$OUT")
        fail "$id" "${NAMES[$i]} inside an async fn: expected ${EXPECT[$i]}, got [${got:-nothing}] (rc=$RC)" \
            "$(grep -m3 -iE 'error|cause' <<<"$OUT")"
    fi
done

run ac07 "{ $BASE }" 'use array
async fn work() {
    let m = array.map_fn([1], fn(v) { throw "CB-THROWN" })
    return 1
}
main {
    try {
        let r = await work()
        print("NOT-THROWN")
    } catch (e) {
        print("CAUGHT=" + string(e))
    }
    print("DONE")
}
'
if [ "$RC" -eq 0 ] && has "CAUGHT=" && has "CB-THROWN" && has "DONE" && ! has "NOT-THROWN"; then
    pass AC-07 "a callback's throw inside an async fn reaches the await's catch"
else
    fail AC-07 "the callback's throw did not reach the await's catch (rc=$RC)" "$OUT"
fi

# Lines: 5 is work's print(v), 12 is clean's print(v) (the program text below).
run ac08 "{ $BASE, $TAINT }" 'use env
use array
async fn work(t) {
    let r = array.map_fn([t], fn(v) {
        print(v)
        return 1
    })
    return 1
}
async fn clean() {
    let r = array.map_fn(["c"], fn(v) {
        print(v)
        return 1
    })
    return 1
}
main {
    let t = env.get("HOME")
    let a = await work(t)
    let b = await clean()
    print("DONE")
}
'
lines=$(flagged_lines)
if has "DONE" && grep -qw 5 <<<"$lines"; then
    pass AC-08 "a tainted callback argument inside an async fn is flagged at its print (line 5)"
else
    fail AC-08 "expected line 5 flagged, flagged [${lines:-none}] (rc=$RC)" "$OUT"
fi
if has "DONE" && ! grep -qw 12 <<<"$lines"; then
    pass AC-08c "CONTROL: the clean callback in the same run is not flagged"
else
    fail AC-08c "the clean callback (line 12) was flagged, or the run did not finish: [${lines:-none}] (rc=$RC)" "$OUT"
fi

run ac09 "{ $BASE, $TAINT }" 'use env
use array
main {
    let t = env.get("HOME")
    let r = array.map_fn([t], fn(v) {
        print(v)
        return 1
    })
    print("DONE")
}
'
lines=$(flagged_lines)
if has "DONE" && grep -qw 6 <<<"$lines"; then
    pass AC-09 "CONTROL: the same tainted callback in main{} is flagged (line 6)"
else
    fail AC-09 "the detector did not flag the main{} callback: [${lines:-none}] (rc=$RC)" "$OUT"
fi

echo ""
echo "PASS=$PASS FAIL=$FAIL UNMEASURABLE=$UNM"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
