#!/usr/bin/env bash
# test_gc_engine_activity.sh -- a collector never clears a value another engine is using (A92)
#
# THE DEFECT
#
# The cycle collector (V-RT-008) marks from ONE engine's roots -- a VM's stack
# and globals -- then sweeps every heap value in the process and clears any
# unreachable list, dict or struct whose refcount is above 1, taking that as a
# reference cycle. Each VM `async fn` runs its own VM on its own thread, with
# its own collector. A value another VM is using is never among the collector's
# roots, and it often has a refcount above 1 for a moment: every read pushes a
# stack copy, and a collection holds a copy of each of its own roots. So one
# VM's collector cleared lists another VM was holding, and the program computed
# with empty lists. Observed on the pre-fix binary, mode off:
#   - main looping beside a sibling async fn: main's own list read 0 elements
#     in 6 of 6 runs; the same loop alone, 3 in 6 of 6
#   - an async fn holding a list beside a sibling async fn: 0 in 3 of 5 runs
#     (5 of 5 in an earlier run)
#   - main calling gc_collect() while an async fn held a list: 0 in 4 of 4
# Now the collector runs only when its engine is the only one executing in the
# process (src/interpreter/cycle_detector.h); otherwise the collection is
# deferred to a later trigger. Concurrency delays a collection; it no longer
# clears a live value.
#
#   GA-01  main's list survives main looping beside a running async fn
#   GA-02  an async fn's list survives a sibling async fn looping
#   GA-03  an async fn's list survives gc_collect() called in main
#   GA-04  CONTROL: the program pattern itself is sound -- the async holder
#          alone, and main looping alone, return 3
#   GA-05  CONTROL: main looping alone still collects (--gc-stats: collections
#          run, none deferred), so "never collect" does not pass this suite
#   GA-06  the collector was asked and deferred while the sibling ran, and ran
#          again once it finished: concurrency delays, it does not disable
#   GA-07  CONTROL: the tree-walker's own gc_collect() still runs (its
#          execute() records itself as the engine on top, A92)
#   GA-08  an async fn's list ARGUMENT survives main collecting while the
#          async fn starts. The worker copies its globals and pushes its
#          arguments before its own run() records it; main's collections in
#          that window cleared the argument in about 96% of calls (observed
#          with the spawn's PendingEngine removed: 360, 357 and 255 of 9000),
#          so the async VM counts as running from the moment it is spawned
# Unit level (tests/unit/gc_engine_activity_test.cpp): the gate per reason to
# defer -- another thread's engine, a pending async engine, a second engine on
# this thread, two VMs on plain threads -- each beside the control that the
# probe list IS cleared once the reason is gone.
#
# Kill table (each mutant rebuilt, suite and unit tests re-run; see the PR):
#   the gate always collects                 -> GA-01, GA-02, GA-03, GA-06 red
#   VM::run() records no activation          -> unit AVmDoesNotSweepWhileAnotherVmRuns
#   the async spawn takes no PendingEngine   -> GA-08 red
#   the gate ignores the engine on top       -> GA-05, GA-07 red (the engine never collects)
# Red first: with the gate forced open (the pre-fix collection behaviour)
# GA-01, GA-02, GA-03, GA-06 and GA-08 fail and the controls pass. GA-05/GA-07
# read --gc-stats fields the pre-fix binary did not print: its line said
# "Collections run" for values cleared, and on the VM it read the idle
# tree-walker's count, so it was 0 on the default engine whatever ran.

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
W=$(mktemp -d "${TMPDIR:-/tmp}/naab_gcactivity_XXXXXX")
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT
printf '%s\n' '{"version":"5.0","mode":"off"}' > "$W/govern.json"

PASS=0; FAIL=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }

# run NAME PROGRAM [flags...] -> OUT, RC
run() {
    local name="$1" prog="$2"; shift 2
    printf '%s' "$prog" > "$W/$name.naab"
    RC=0; OUT=$(cd "$W" && timeout 120s "$NAAB" run "$@" "$name.naab" 2>&1) || RC=$?
    OUT="${OUT//$'\r'/}"
}
has() { grep -q -F -- "$1" <<<"$OUT"; }
brief() { grep -v 'governance\] Loaded' <<<"$OUT" | head -c 300 | tr '\n' ' '; }
# gc FIELD -> the number --gc-stats printed for FIELD ("Collections run", "deferred")
gc() { sed -n "s/.*$1: \([0-9][0-9]*\).*/\1/p" <<<"$OUT" | head -1; }

SPIN=$'async fn spin(n) {\n    let s = 0\n    for i in 0..n {\n        s = s + 1\n    }\n    return s\n}\n'
HOLDER=$'async fn holder(n) {\n    let a = [1, 2, 3]\n    let s = 0\n    for i in 0..n {\n        s = s + a.length()\n    }\n    return a.length()\n}\n'

echo "=== The cycle collector and concurrent engines (A92) ==="

# GA-01: main itself collects at its loop's back-edges while the sibling runs.
P01="$SPIN"$'fn beside() {\n    let a = [1, 2, 3]\n    let f = spin(400000)\n    let s = 0\n    for i in 0..400000 {\n        s = s + 1\n    }\n    let r = await f\n    return a.length()\n}\nmain {\n    print("GA01=" + string(beside()))\n}\n'
bad=""
for i in 1 2 3; do
    run ga01 "$P01"
    if ! { [ "$RC" -eq 0 ] && has "GA01=3"; }; then bad="run $i: rc=$RC $(brief)"; break; fi
done
if [ -z "$bad" ]; then
    pass GA-01 "main's list keeps its 3 elements while main loops beside a running async fn (3 runs)"
else
    fail GA-01 "a sibling's collector, or main's, cleared main's list" "$bad"
fi

# GA-02: two async fns; the holder only reads its list, the sibling loops.
P02="$SPIN$HOLDER"$'main {\n    let h = holder(400000)\n    let f = spin(200000)\n    let r = await f\n    print("GA02=" + string(await h))\n}\n'
bad=""
for i in 1 2 3; do
    run ga02 "$P02"
    if ! { [ "$RC" -eq 0 ] && has "GA02=3"; }; then bad="run $i: rc=$RC $(brief)"; break; fi
done
if [ -z "$bad" ]; then
    pass GA-02 "an async fn's list survives a sibling async fn's collections (3 runs)"
else
    fail GA-02 "the sibling's collector cleared the holder's list" "$bad"
fi

# GA-03: an explicit collection in main while the holder runs.
P03="$HOLDER"$'main {\n    let h = holder(200000)\n    for i in 0..2000 {\n        gc_collect()\n    }\n    print("GA03=" + string(await h))\n}\n'
bad=""
for i in 1 2 3; do
    run ga03 "$P03"
    if ! { [ "$RC" -eq 0 ] && has "GA03=3"; }; then bad="run $i: rc=$RC $(brief)"; break; fi
done
if [ -z "$bad" ]; then
    pass GA-03 "gc_collect() in main leaves a running async fn's list intact (3 runs)"
else
    fail GA-03 "main's gc_collect() cleared the async fn's list" "$bad"
fi

# GA-04: the same holder and loop with nothing beside them.
P04="$HOLDER"$'fn alone() {\n    let a = [1, 2, 3]\n    let s = 0\n    for i in 0..400000 {\n        s = s + 1\n    }\n    return a.length()\n}\nmain {\n    print("GA04H=" + string(await holder(400000)))\n    print("GA04M=" + string(alone()))\n}\n'
run ga04 "$P04"
if [ "$RC" -eq 0 ] && has "GA04H=3" && has "GA04M=3"; then
    pass GA-04 "CONTROL: the holder alone and main's loop alone each return 3"
else
    fail GA-04 "the programs themselves should return 3 with nothing beside them" "rc=$RC $(brief)"
fi

# GA-05: main alone collects at all.
P05=$'main {\n    let a = [1, 2, 3]\n    let s = 0\n    for i in 0..100000 {\n        s = s + 1\n    }\n    print("GA05=" + string(a.length()))\n}\n'
run ga05 "$P05" --gc-stats
runs=$(gc "Collections run"); deferred=$(gc "deferred")
if [ "$RC" -eq 0 ] && has "GA05=3" && [ "${runs:-0}" -gt 0 ] && [ "${deferred:-x}" = "0" ]; then
    pass GA-05 "CONTROL: main alone still collects ($runs collections, none deferred)"
else
    fail GA-05 "main alone should collect and defer nothing" "rc=$RC runs=${runs:-?} deferred=${deferred:-?} $(brief)"
fi

# GA-06: deferred while the sibling runs, collected after it is awaited.
P06="$SPIN"$'fn phases() {\n    let a = [1, 2, 3]\n    let f = spin(300000)\n    let s = 0\n    for i in 0..300000 {\n        s = s + 1\n    }\n    let r = await f\n    let during = a.length()\n    for i in 0..100000 {\n        s = s + 1\n    }\n    return during * 10 + a.length()\n}\nmain {\n    print("GA06=" + string(phases()))\n}\n'
run ga06 "$P06" --gc-stats
runs=$(gc "Collections run"); deferred=$(gc "deferred")
if [ "$RC" -eq 0 ] && has "GA06=33" && [ "${deferred:-0}" -gt 0 ] && [ "${runs:-0}" -gt 0 ]; then
    pass GA-06 "collections deferred while the sibling ran ($deferred) and ran once it finished ($runs); the list kept 3"
else
    fail GA-06 "expected deferred > 0, run > 0 and the list intact" "rc=$RC runs=${runs:-?} deferred=${deferred:-?} $(brief)"
fi

# GA-07: the tree-walker (allowed under mode off) collects on gc_collect().
P07=$'main {\n    let a = [1, 2, 3]\n    gc_collect()\n    print("GA07=" + string(a.length()))\n}\n'
run ga07 "$P07" --tree-walk --gc-stats
runs=$(gc "Collections run"); deferred=$(gc "deferred")
if [ "$RC" -eq 0 ] && has "GA07=3" && [ "${runs:-0}" -gt 0 ] && [ "${deferred:-x}" = "0" ]; then
    pass GA-07 "CONTROL: the tree-walker's gc_collect() runs ($runs collection, none deferred)"
else
    fail GA-07 "the tree-walker's gc_collect() should run" "rc=$RC runs=${runs:-?} deferred=${deferred:-?} $(brief)"
fi

# GA-08: main collects while each async fn is still starting.
P08=$'async fn probe(x) {\n    return x.length()\n}\nmain {\n    let n = 0\n    for i in 0..500 {\n        let f = probe([1, 2, 3])\n        gc_collect()\n        gc_collect()\n        n = n + await f\n    }\n    print("GA08=" + string(n))\n}\n'
bad=""
for i in 1 2 3; do
    run ga08 "$P08"
    if ! { [ "$RC" -eq 0 ] && has "GA08=1500"; }; then bad="run $i: rc=$RC $(brief)"; break; fi
done
if [ -z "$bad" ]; then
    pass GA-08 "500 async calls each see their 3-element argument while main collects (3 runs)"
else
    fail GA-08 "main's collections cleared an async fn's argument before it started" "$bad"
fi

echo ""
echo "GC engine activity: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
