#!/usr/bin/env bash
# test_debug_vm_scope.sh -- debug.env/stack/snapshot/trace on the VM (F46).
#
# The debug module read scope and stack from the TREE-WALKER (the idle
# interpreter main.cpp builds before it chooses the engine), so on the VM, the
# default engine, debug.env() was {}, debug.stack() [], debug.snapshot()
# captured 0 variables and debug.trace() printed "<repl>" as its location
# (observed on d32f8603). They now answer from the VM's own frames:
# VM::debugStack() (each frame's function and executing line) and
# VM::debugScope() (the running function's locals, by the instruction range
# each is live in, recorded by the compiler's addLocal/popLocal).
#
# Two differences from the tree-walker, kept and recorded: a stack entry names
# the line EXECUTING in that frame (the tree-walker names the function's
# declaration line), and the top level is listed as "<script>".
#
# Taint: what debug.env() copies out of a tainted local, and what
# debug.compare() returns from a snapshot that held one, is tainted data.
# Making the VM answer opened that route (observed while building this: a
# tainted local read back through debug.env() reached a print sink, exit 0);
# both now taint their result through the return flag every stdlib call
# route reads (DV-T arms), and a NAME read from such a dict is tainted too
# (the whole dict is; a stricter answer, not a leak).
#
#   DV-00   CONTROL: the harness runs a program.
#   DV-01   debug.env() in a function names its parameter and locals, the
#           same set as the tree-walker (CONTROL arm: the tree-walker).
#   DV-02   a block's local is listed inside the block and not after it.
#   DV-03   debug.stack() lists the function, innermost last, with its file.
#   DV-04   debug.snapshot()/compare() see the change of a local.
#   DV-05   debug.trace() names the function and line it was called from.
#   DV-06   inside two concurrent async fns, debug.env() names each worker's
#           own locals, and main's in main (each answers from its own VM).
#           The hooks were first written by every VM constructor, a data
#           race the TSan job reported on b7c18a91; they are now registered
#           once and atomic, and this suite is in tests/tsan/suites.txt.
#   DV-07   four concurrent async fns each use debug.snapshot/compare/watch/
#           timer, 200 times each: each compare sees its own change. Under
#           ThreadSanitizer it is the check that the module's tables, under
#           one lock since Bundle 19 PR 4, are not raced. The loop is what
#           makes it one: with the lock removed, the earlier one-call-each
#           program drew no report in 3 runs (the VM's other locks order those
#           few accesses), and this one 166 and 161 reports in 2 runs; with
#           the lock, 0 in 3 (2026-11-27, clang 18, observed).
#   DV-T0   CONTROL: printing the tainted local directly is refused (exit 3),
#           so the fixture taints and the sink fires.
#   DV-T1   a tainted local read back through debug.env() is refused at the
#           sink; DV-T1m the same through `debug.env` without parentheses
#           (the member route); DV-T2 through a copy of the dict.
#   DV-T3   debug.compare() of a snapshot holding tainted data is refused.
#   DV-T4   CONTROL: a clean local through debug.env(), and a clean compare,
#           print (exit 0) under the same config.
#
# Kill table (mutants of src/vm/vm.cpp, src/vm/compiler.cpp and
# src/stdlib/debug_impl.cpp, run by hand):
#   F1 VM::debugScope() returns nothing              -> DV-01, DV-02, DV-04
#   F2 VM::debugStack() returns nothing              -> DV-03, DV-05
#   F3 popLocal() does not close a local's range     -> DV-02
#   F4 debug.env() sets no return taint              -> DV-T1, DV-T1m, DV-T2
#   F5 the member route does not read the flag       -> DV-T1m
#   F6 debug.compare() sets no return taint          -> DV-T3
#   F7 debugScope() reports every local clean        -> DV-T1, DV-T1m, DV-T2, DV-T3
#   F8 the VM does not register its hooks            -> DV-01..DV-05
#   F9 debugScope() reports every local tainted      -> DV-T4
#   F10 the hooks answer from the first VM that asked, -> DV-06
#       not the one running on the calling thread
# The registration race itself is not a mutant here: only a ThreadSanitizer
# build shows it (tests/tsan/run_tsan.sh, red on b7c18a91, green after); the
# same holds for DV-07's lock (removing it is a TSan mutant: each acquisition
# given a fresh mutex, run under TSan by hand on 2026-11-27, red as above).

set -uo pipefail
PASS=0; FAIL=0
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
W="$(mktemp -d "${TMPDIR:-/tmp}/naab_dbgvm.XXXXXX")"
source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT
ok()  { echo "  PASS $1: $2"; PASS=$((PASS+1)); }
bad() { echo "  FAIL $1: $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -8 | sed 's/^/      /'; FAIL=$((FAIL+1)); }
[ -x "$NAAB" ] || { echo "UNMEASURABLE: $NAAB not built"; exit 1; }

mkdir -p "$W/free"
nr() { OUT=$(cd "$W/free" && timeout 60 "$NAAB" run "$1" --no-governance "${@:2}" 2>&1); RC=$?; }
# A key list printed as [a, b] -> "a b " sorted (each engine orders its own way).
keyset() { sed -n "s/^$1=//p" <<<"$2" | head -1 | tr -d '[]" ' | tr ',' '\n' | sed '/^$/d' | sort | tr '\n' ' '; }

printf 'main {\n    print("ALIVE")\n}\n' > "$W/free/alive.naab"; nr alive.naab
if [ "$RC" -eq 0 ] && grep -q '^ALIVE$' <<<"$OUT"; then ok DV-00 "CONTROL: the harness runs a program"
else bad DV-00 "the harness did not run (rc=$RC)" "$OUT"; exit 1; fi

cat > "$W/free/scope.naab" <<'EOF'
fn inner(n) {
    let local_in = n + 1
    if true {
        let blk = 5
        print("IN=" + string(debug.keys(debug.env())))
    }
    print("AFTER=" + string(debug.keys(debug.env())))
    print("STACK=" + string(debug.stack()))
    return local_in
}
main {
    inner(3)
}
EOF
nr scope.naab; vm="$OUT"; vrc=$RC
nr scope.naab --tree-walk; tw="$OUT"; trc=$RC
if [ "$vrc" -eq 0 ] && [ "$trc" -eq 0 ] && [ "$(keyset IN "$vm")" = "blk local_in n " ] \
   && [ "$(keyset IN "$tw")" = "blk local_in n " ]; then
    ok DV-01 "debug.env() names the parameter and locals on the VM, as on the tree-walker"
else bad DV-01 "debug.env() in a function (vm rc=$vrc '$(keyset IN "$vm")', tw rc=$trc '$(keyset IN "$tw")')" "$vm"; fi
if [ "$(keyset AFTER "$vm")" = "local_in n " ]; then ok DV-02 "a block's local is gone after the block"
else bad DV-02 "after the block: '$(keyset AFTER "$vm")' (want 'local_in n ')" "$vm"; fi
st=$(sed -n 's/^STACK=//p' <<<"$vm" | head -1)
if grep -q 'at inner (.*scope\.naab:8)\]$' <<<"$st" && grep -q 'at <script> (' <<<"$st"; then
    ok DV-03 "debug.stack() lists the function last, with its file and executing line"
else bad DV-03 "debug.stack() on the VM: '$st'" "$vm"; fi

cat > "$W/free/snap.naab" <<'EOF'
fn f() {
    let w = "plain"
    debug.snapshot("a")
    w = "changed"
    debug.snapshot("b")
    print("CMP=" + debug.compare("a", "b"))
}
main {
    f()
}
EOF
nr snap.naab
if [ "$RC" -eq 0 ] && grep -q 'Changed: w: "plain" -> "changed"' <<<"$OUT" && grep -q "captured (1 variables)" <<<"$OUT"; then
    ok DV-04 "debug.snapshot()/compare() see the local's change"
else bad DV-04 "snapshot/compare (rc=$RC)" "$OUT"; fi

# DV-06: inside concurrent async fns each worker answers from ITS OWN VM.
# Every async fn runs a fresh VM on a worker thread; the hooks are shared by
# all of them, so this is also the read path ThreadSanitizer checks when the
# TSan job runs this suite (tests/tsan/suites.txt).
cat > "$W/free/async.naab" <<'EOF'
async fn worker(tag) {
    let mine = tag + "_local"
    return tag + ":" + string(debug.keys(debug.env()))
}
main {
    let outer_only = 1
    let f1 = worker("w1")
    let f2 = worker("w2")
    print("A1=" + await f1)
    print("A2=" + await f2)
    print("M=" + string(debug.keys(debug.env())))
}
EOF
nr async.naab
a1=$(sed -n 's/^A1=w1://p' <<<"$OUT" | head -1 | tr -d '[] ' | tr ',' '\n' | sort | tr '\n' ' ')
a2=$(sed -n 's/^A2=w2://p' <<<"$OUT" | head -1 | tr -d '[] ' | tr ',' '\n' | sort | tr '\n' ' ')
if [ "$RC" -eq 0 ] && [ "$a1" = "mine tag " ] && [ "$a2" = "mine tag " ] && [ "$(keyset M "$OUT")" = "f1 f2 outer_only " ]; then
    ok DV-06 "debug.env() in concurrent async fns names each worker's own locals, and main's in main"
else bad DV-06 "async scopes (rc=$RC, w1 '$a1', w2 '$a2', main '$(keyset M "$OUT")')" "$OUT"; fi

# DV-07: the module's snapshot, watch and timer tables are shared by every
# engine thread; four concurrent async fns each use them. On a normal build
# this checks the answers; under ThreadSanitizer (the TSan job runs this suite)
# it is the race check -- the tables had no lock (F46's record).
cat > "$W/free/conc.naab" <<'EOF'
async fn worker(tag) {
    let w = tag
    let i = 0
    while i < 200 {
        debug.snapshot(tag + string(i))
        debug.watch(tag + string(i), w)
        debug.timer(tag + string(i))
        i = i + 1
    }
    debug.snapshot(tag + "a")
    w = tag + "!"
    debug.snapshot(tag + "b")
    let c = debug.compare(tag + "a", tag + "b")
    debug.watch(tag, w)
    debug.timer(tag)
    debug.timer(tag)
    return c
}
main {
    let f1 = worker("p")
    let f2 = worker("q")
    let f3 = worker("r")
    let f4 = worker("s")
    let all = [await f1, await f2, await f3, await f4]
    let n = 0
    for c in all {
        if c.contains("Changed: w") {
            n = n + 1
        }
    }
    print("CHANGED=" + string(n))
}
EOF
nr conc.naab
if [ "$RC" -eq 0 ] && grep -qx 'CHANGED=4' <<<"$OUT"; then
    ok DV-07 "four concurrent async fns each snapshot, compare, watch and time their own values"
else bad DV-07 "concurrent debug tables (rc=$RC)" "$OUT"; fi

printf 'fn inner(n) {\n    let x = debug.trace(n + 1)\n    return x\n}\nmain {\n    inner(3)\n}\n' > "$W/free/trace.naab"
nr trace.naab
if [ "$RC" -eq 0 ] && grep -q '^\[TRACE\]   at inner (.*trace\.naab:2) -> 4$' <<<"$OUT"; then
    ok DV-05 "debug.trace() names the function and line"
else bad DV-05 "debug.trace() location (rc=$RC)" "$OUT"; fi

# ---- taint: what the debug module copies out of a tainted local ----
mkdir -p "$W/t"
printf '{"version":"3.0","mode":"enforce","security":{"sandbox_level":"elevated"},"taint_tracking":{"enabled":true,"level":"hard","sources":["env.get"],"sinks":["print"]}}\n' > "$W/t/govern.json"
trun() {
    printf 'use env\nfn f() {\n%s\n}\nmain {\n    f()\n}\n' "$2" > "$W/t/$1.naab"
    OUT=$(cd "$W/t" && DV_T=tval timeout 60 "$NAAB" run "$1.naab" 2>&1); RC=$?
}
refused() { [ "$RC" -eq 3 ] && ! grep -q 'tval' <<<"$OUT" && grep -q 'taint_tracking.sink_violation' <<<"$OUT"; }

trun t0 '    let v = env.get("DV_T")
    print("DIRECT=" + v)'
if refused; then ok DV-T0 "CONTROL: printing the tainted local directly is refused (exit 3)"
else bad DV-T0 "the direct print was not refused (rc=$RC) -- every DV-T arm is UNMEASURABLE" "$OUT"; fi

trun t1 '    let v = env.get("DV_T")
    let d = debug.env()
    print("LEAK=" + d["v"])'
if refused; then ok DV-T1 "a tainted local read back through debug.env() is refused at the sink"
else bad DV-T1 "debug.env() laundered the tainted local (rc=$RC)" "$OUT"; fi

trun t1m '    let v = env.get("DV_T")
    let d = debug.env
    print("LEAK=" + d["v"])'
if refused; then ok DV-T1m "the same through debug.env without parentheses (the member route)"
else bad DV-T1m "the member route laundered the tainted local (rc=$RC)" "$OUT"; fi

trun t2 '    let v = env.get("DV_T")
    let d = debug.env()
    let e = d
    let y = e["v"]
    print("LEAK=" + y)'
if refused; then ok DV-T2 "through a copy of the dict, still refused"
else bad DV-T2 "a copy of the dict laundered the tainted local (rc=$RC)" "$OUT"; fi

trun t3 '    let w = "plain"
    debug.snapshot("a")
    let v = env.get("DV_T")
    debug.snapshot("b")
    print("CMP=" + debug.compare("a", "b"))'
if refused; then ok DV-T3 "debug.compare() of a snapshot holding tainted data is refused at the sink"
else bad DV-T3 "debug.compare() laundered the tainted value (rc=$RC)" "$OUT"; fi

trun t4 '    let w = "plain"
    let d = debug.env()
    print("CLEAN=" + d["w"])
    debug.snapshot("c1")
    w = "changed"
    debug.snapshot("c2")
    print("CCMP=" + debug.compare("c1", "c2"))'
if [ "$RC" -eq 0 ] && grep -q '^CLEAN=plain$' <<<"$OUT" && grep -q '^CCMP=  Changed: w' <<<"$OUT"; then
    ok DV-T4 "CONTROL: clean data through debug.env() and compare() prints under the same config"
else bad DV-T4 "the clean read was refused or missing (rc=$RC)" "$OUT"; fi

echo
echo "debug on the VM: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
