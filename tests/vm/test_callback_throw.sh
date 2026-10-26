#!/usr/bin/env bash
# ============================================================
# test_callback_throw.sh -- a NAAb callback that throws through native code (A41)
#
# THE DEFECT
#
# A callback -- a sort comparator, array.map_fn's function, an agent tool --
# runs in a NESTED VM::run(), entered from native code (std::sort, the array
# module, the agent tool loop) that sits on the C++ stack between the callback
# and its caller's frames. Two things went wrong when the callback threw:
#
#   * the nested run() routed the throw to exception_handlers_.back() without
#     asking whose handler it was: a `try` in the CALLER's frame caught it, and
#     the callback's dispatch loop went on to execute the caller's catch block
#     and the rest of the caller, with the native code still waiting below;
#   * callNaabFunction() restored the VM (frames, stack, stop level, depth,
#     taint) only after run() RETURNED, so a throw left the callback's frames
#     and stack slots behind for whatever ran next.
#
# The visible results, all on the VM only (the tree-walker is the oracle here):
# a run that finished its catch block and then died with "Uncaught exception"
# or "callNaabFunction: value is not a function" after main; the script's next
# r.get(...) after a throwing agent tool dispatched as agent.get; and after 64
# caught throws, "Maximum callback nesting depth exceeded" for every callback.
#
# A second defect in the same family, both engines: array.sort(arr, cmp) and
# array.sorted(arr, cmp) (the MODULE form, array_impl.cpp) replaced anything
# the comparator threw with "comparator must return a number" -- including a
# HARD governance block, which became catchable and the run exited 0.
#
# Every arm runs the VM and the tree-walker; a callback arm passes only when
# the run completes with the thrown value in the caller's catch and the
# caller's locals intact afterwards.
#
#   CT-00  harness control: returning callbacks complete on both engines
#   CT-01..CT-09  a throwing callback, caught in main, on each route: .sort,
#          .sorted (VM only: the tree-walker has no .sorted method), .for_each,
#          .find, array.map_fn, array.filter_fn, array.reduce_fn, array.sort
#          and array.sorted (module form)
#   CT-10  the throw comes from a function the callback calls
#   CT-11  CONTROL: a callback's own try still catches its own throw
#   CT-12  a callback's try catches a throw from a callback IT started
#          (one native level down), and the outer callback carries on
#   CT-13  100 caught throws, then a working callback (the depth counter)
#   CT-14  a thrown dict arrives as the dict
#   CT-15  uncaught: the run fails, with the thrown text, and stops there
#   CT-16  taint survives the throw across the callback (CT-16p: the direct
#          throw is flagged too; CT-16c: an untainted throw is not)
#   CT-17  a HARD block inside an array.sort comparator stays HARD (exit 3,
#          catch not entered); CT-17c: the same read called directly
#   CT-18  CONTROL (V-UB-001): a comparator returning a string is still
#          "must return a number"
#   CT-19  a throwing agent tool: the reply is returned, the next r.get()
#          works; CT-19c: a returning tool. VM only (tree-walker tools: F36);
#          skipped on Windows, where the agent stub hangs the runner
#   CT-20  contract golden tests after a throwing one still run (clean
#          before the fix, kept as the regression arm for that route). Runs
#          before CT-19, which installs a trusted key, so it needs no signature
#   CT-21  a NATIVE error inside a comparator is caught in main, its message
#          carrying the stack trace once
# ============================================================
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
source "$REPO/tests/helpers/trust_setup.sh"; setup_isolated_trust
source "$REPO/tests/helpers/stub_platform.sh" 2>/dev/null
source "$REPO/tests/helpers/stub_launch.sh"

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP [$1] UNMEASURABLE: $2"; }

[ -x "$NAAB" ] || { echo "naab-lang not built"; exit 1; }
T="$(mktemp -d)"
cleanup() { stop_stub 2>/dev/null; teardown_isolated_trust; [ -n "${KEEP_TMP:-}" ] || rm -rf "$T"; }
trap cleanup EXIT
printf '{"version":"4.0","mode":"enforce","security":{"sandbox_level":"elevated"}}\n' > "$T/govern.json"
# The tree-walker, the oracle here, is refused under enforce (CT-TW), so its
# runs are in $T/tw beside the same config in mode audit: these arms are about
# where an exception lands, and audit keeps the checks the taint arms read.
mkdir -p "$T/tw"
printf '{"version":"4.0","mode":"audit","security":{"sandbox_level":"elevated"}}\n' > "$T/tw/govern.json"

# run NAME ENGINE PROGRAM -> OUT, RC   (in $T, under $T/govern.json; tw: $T/tw)
# Every capture drops CR: a native Windows build may end its lines in CRLF, and
# the arms match whole lines (grep -x).
run() {
    local d="$T"; [ "$2" = tw ] && d="$T/tw"
    printf '%s\n' "$3" > "$d/$1.naab"
    local a=("$NAAB" run); [ "$2" = tw ] && a+=(--tree-walk); a+=("$1.naab")
    OUT="$(cd "$d" && timeout 30s "${a[@]}" 2>&1)"; RC=$?; OUT="${OUT//$'\r'/}"
}
show() { grep -v '^\[governance\] Loaded\|^-- Governance' <<<"$OUT" | head -4 | tr '\n' '|'; }

# cb_arm ID LABEL ENGINES EXPECT_CAUGHT CALL
# CALL is one statement that throws "boom" from a callback. The program
# catches it in main, then runs a working callback and reads a local: the
# arm passes only if all three happen and nothing fails after main.
cb_arm() {
    local id=$1 label=$2 engines=$3 want=$4 call=$5
    local prog="use array
main {
    let d = {\"k\": 7}
    let a = [3, 1, 2]
    try {
        $call
        print(\"NOT_REACHED\")
    } catch (e) {
        print(\"CAUGHT=\" + string(e))
    }
    a.sort(fn(x, y) { return x - y })
    print(\"AFTER k=\" + string(d.get(\"k\")) + \" a0=\" + string(a[0]))
}"
    for eng in $engines; do
        run "$id-$eng" "$eng" "$prog"
        if [ "$RC" -eq 0 ] && grep -qx "CAUGHT=$want" <<<"$OUT" && grep -qx 'AFTER k=7 a0=1' <<<"$OUT" \
           && ! grep -q 'NOT_REACHED\|^Error' <<<"$OUT"; then
            ok "$id/$eng" "$label: caught \"$want\", caller intact, run completes"
        else
            bad "$id/$eng" "$label (rc=$RC)" "$(show)"
        fi
    done
}

echo "=== A41: callbacks that throw through native code ==="

# CT-00
for eng in vm tw; do
    run "ct00-$eng" "$eng" 'use array
main {
    let d = {"k": 7}
    let a = [3, 1, 2]
    a.sort(fn(x, y) { return x - y })
    a.for_each(fn(x) { let z = x })
    let m = array.map_fn(a, fn(x) { return x * 2 })
    print("OK k=" + string(d.get("k")) + " a0=" + string(a[0]) + " m0=" + string(m[0]))
}'
    if [ "$RC" -eq 0 ] && grep -qx 'OK k=7 a0=1 m0=2' <<<"$OUT"; then
        ok "CT-00/$eng" "control: returning callbacks complete"
    else
        bad "CT-00/$eng" "control: returning callbacks did not complete (rc=$RC)" "$(show)"
    fi
done

cb_arm CT-01 ".sort comparator"      "vm tw" boom 'a.sort(fn(x, y) { throw "boom" })'
cb_arm CT-02 ".sorted comparator"    "vm"    boom 'let s = a.sorted(fn(x, y) { throw "boom" })'
cb_arm CT-03 ".for_each callback"    "vm tw" boom 'a.for_each(fn(x) { throw "boom" })'
cb_arm CT-04 ".find predicate"       "vm tw" boom 'let f = a.find(fn(x) { throw "boom" })'
cb_arm CT-05 "array.map_fn"          "vm tw" boom 'let m = array.map_fn(a, fn(x) { throw "boom" })'
cb_arm CT-06 "array.filter_fn"       "vm tw" boom 'let m = array.filter_fn(a, fn(x) { throw "boom" })'
cb_arm CT-07 "array.reduce_fn"       "vm tw" boom 'let m = array.reduce_fn(a, fn(acc, x) { throw "boom" }, 0)'
cb_arm CT-08 "array.sort (module)"   "vm tw" boom 'let m = array.sort(a, fn(x, y) { throw "boom" })'
cb_arm CT-09 "array.sorted (module)" "vm tw" boom 'let m = array.sorted(a, fn(x, y) { throw "boom" })'

# CT-10 -- the throw is two NAAb frames deep inside the callback.
for eng in vm tw; do
    run "ct10-$eng" "$eng" 'fn deep(x) { throw "deep-boom" }
fn mid(x) { return deep(x) }
main {
    let d = {"k": 7}
    try { [1, 2].for_each(fn(x) { mid(x) }) } catch (e) { print("CAUGHT=" + string(e)) }
    print("AFTER k=" + string(d.get("k")))
}'
    if [ "$RC" -eq 0 ] && grep -qx 'CAUGHT=deep-boom' <<<"$OUT" && grep -qx 'AFTER k=7' <<<"$OUT" && ! grep -q '^Error' <<<"$OUT"; then
        ok "CT-10/$eng" "a throw from a function the callback calls is caught in main"
    else
        bad "CT-10/$eng" "throw from inside the callback's callee (rc=$RC)" "$(show)"
    fi
done

# CT-21 -- a NATIVE error inside the callback (not a NAAb throw) takes the
# VMException route out of the nested run(). The caught message must carry its
# stack trace once: converting it at the boundary appended the outer frames a
# second time (VM only -- the tree-walker formats native errors differently).
for eng in vm tw; do
    run "ct21-$eng" "$eng" 'main {
    let d = {"k": 7}
    let msg = ""
    try { [2, 1].sort(fn(a, b) { return int("not-a-number") }) } catch (e) { msg = string(e) }
    print("CAUGHT=" + string(msg != ""))
    print("SCRIPT_FRAMES=" + string(len(string.split(msg, "at <script>()")) - 1))
    print("AFTER k=" + string(d.get("k")))
}'
    frames="$(sed -n 's/^SCRIPT_FRAMES=//p' <<<"$OUT")"
    if [ "$RC" -eq 0 ] && grep -qx 'CAUGHT=true' <<<"$OUT" && grep -qx 'AFTER k=7' <<<"$OUT" \
       && { [ "$eng" = tw ] || [ "$frames" = 1 ]; }; then
        ok "CT-21/$eng" "a native error inside a comparator is caught in main; caller intact${frames:+ (script frames in message: $frames)}"
    else
        bad "CT-21/$eng" "native error inside a comparator (rc=$RC, script frames=$frames)" "$(show)"
    fi
done

# CT-11 -- control: the floor must not hide a handler the callback pushed.
for eng in vm tw; do
    run "ct11-$eng" "$eng" 'main {
    let n = 0
    [1, 2, 3].for_each(fn(x) {
        try { throw "inner" } catch (e) { print("INNER " + string(x) + " " + string(e)) }
    })
    print("AFTER")
}'
    if [ "$RC" -eq 0 ] && [ "$(grep -c '^INNER [123] inner$' <<<"$OUT")" -eq 3 ] && grep -qx AFTER <<<"$OUT"; then
        ok "CT-11/$eng" "control: a callback's own try catches its own throw, every element"
    else
        bad "CT-11/$eng" "a callback's own try did not catch (rc=$RC)" "$(show)"
    fi
done

# CT-12 -- the handler is in the callback, the throw one native level below it.
for eng in vm tw; do
    run "ct12-$eng" "$eng" 'main {
    let seen = []
    [1, 2, 3].for_each(fn(x) {
        try {
            [2, 1].sort(fn(p, q) { throw "cmp-" + string(x) })
        } catch (e) {
            print("MID " + string(e))
        }
    })
    print("AFTER")
}'
    if [ "$RC" -eq 0 ] && [ "$(grep -c '^MID cmp-[123]$' <<<"$OUT")" -eq 3 ] && grep -qx AFTER <<<"$OUT" && ! grep -q '^Error' <<<"$OUT"; then
        ok "CT-12/$eng" "a callback catches a throw from a callback it started; all 3 elements visited"
    else
        bad "CT-12/$eng" "middle-level catch (rc=$RC)" "$(show)"
    fi
done

# CT-13 -- the depth counter (MAX_RUN_DEPTH = 64) must not leak per throw.
for eng in vm tw; do
    run "ct13-$eng" "$eng" 'main {
    let a = [3, 1, 2]
    let caught = 0
    let i = 0
    while i < 100 {
        try { a.for_each(fn(x) { throw "boom" }) } catch (e) { caught = caught + 1 }
        i = i + 1
    }
    a.sort(fn(x, y) { return x - y })
    print("CAUGHT=" + string(caught) + " a0=" + string(a[0]))
}'
    if [ "$RC" -eq 0 ] && grep -qx 'CAUGHT=100 a0=1' <<<"$OUT"; then
        ok "CT-13/$eng" "100 caught callback throws, then a working callback"
    else
        bad "CT-13/$eng" "callbacks after 100 caught throws (rc=$RC)" "$(show)"
    fi
done

# CT-14 -- value fidelity: the catch gets the thrown dict.
for eng in vm tw; do
    run "ct14-$eng" "$eng" 'main {
    try { [2, 1].sort(fn(a, b) { throw {"code": 42} }) } catch (e) {
        print("TYPE=" + type(e) + " CODE=" + string(e.get("code")))
    }
}'
    if [ "$RC" -eq 0 ] && grep -qx 'TYPE=dict CODE=42' <<<"$OUT"; then
        ok "CT-14/$eng" "a thrown dict arrives as the dict"
    else
        bad "CT-14/$eng" "thrown dict across the callback (rc=$RC)" "$(show)"
    fi
done

# CT-15 -- uncaught: fails with the thrown text and stops.
for eng in vm tw; do
    run "ct15-$eng" "$eng" 'main {
    [1, 2].for_each(fn(x) { throw "boom-uncaught" })
    print("NOT_REACHED")
}'
    if [ "$RC" -eq 1 ] && grep -q 'boom-uncaught' <<<"$OUT" && ! grep -q NOT_REACHED <<<"$OUT"; then
        ok "CT-15/$eng" "an uncaught callback throw fails the run (rc=1) with its text"
    else
        bad "CT-15/$eng" "uncaught callback throw (rc=$RC)" "$(show)"
    fi
done

# CT-16 -- taint across the boundary (V-TAINT-THROW). Presence, not counts:
# the two engines print a violation over a different number of lines.
TD="$T/taint"; mkdir -p "$TD"
printf '%s\n' '{"version":"4.0","mode":"enforce","security":{"sandbox_level":"elevated"},
 "taint_tracking":{"enabled":true,"level":"advisory","sources":["env.get"],"sinks":["file.write"],"sanitizers":["sanitize_"]}}' > "$TD/govern.json"
# the tree-walker beside an audit copy (it is refused under enforce)
mkdir -p "$TD/tw"; sed 's/"mode":"enforce"/"mode":"audit"/' "$TD/govern.json" > "$TD/tw/govern.json"
taint_run() { # NAME ENGINE BODY
    local d="$TD"; [ "$2" = tw ] && d="$TD/tw"
    printf 'use env\nuse file\nmain {\n    let s = env.get("A41_SRC")\n%s\n    print("DONE")\n}\n' "$3" > "$d/$1.naab"
    local a=("$NAAB" run); [ "$2" = tw ] && a+=(--tree-walk); a+=("$1.naab")
    OUT="$(cd "$d" && A41_SRC=value timeout 30s "${a[@]}" 2>&1)"; RC=$?; OUT="${OUT//$'\r'/}"
}
flagged() { grep -qiE 'sink_violation|Taint tracking violation' <<<"$OUT"; }
for eng in vm tw; do
    taint_run "p-$eng" "$eng" '    try { throw s } catch (e) { file.write("out.txt", e) }'
    if [ "$RC" -eq 0 ] && flagged && grep -qx DONE <<<"$OUT"; then
        ok "CT-16p/$eng" "control: a tainted value thrown and caught directly is flagged at the sink"
    else
        bad "CT-16p/$eng" "control: direct tainted throw not flagged (rc=$RC)" "$(show)"
    fi
    taint_run "t-$eng" "$eng" '    let a = [2, 1]
    try { a.sort(fn(x, y) { throw s }) } catch (e) { file.write("out.txt", e) }'
    if [ "$RC" -eq 0 ] && flagged && grep -qx DONE <<<"$OUT"; then
        ok "CT-16/$eng" "a tainted value thrown from a comparator is still flagged at the sink"
    else
        bad "CT-16/$eng" "taint across the callback throw (rc=$RC)" "$(show)"
    fi
    taint_run "c-$eng" "$eng" '    let a = [2, 1]
    try { a.sort(fn(x, y) { throw "literal" }) } catch (e) { file.write("out.txt", e) }'
    if [ "$RC" -eq 0 ] && ! flagged && grep -qx DONE <<<"$OUT"; then
        ok "CT-16c/$eng" "control: an untainted value thrown the same way is not flagged"
    else
        bad "CT-16c/$eng" "untainted callback throw (rc=$RC)" "$(show)"
    fi
done

# CT-17 -- a HARD block inside a module-form comparator stays HARD.
HD="$T/hard"; mkdir -p "$HD"; echo SECRET > "$HD/secret.txt"
printf '%s\n' '{"version":"4.0","mode":"enforce","security":{"sandbox_level":"elevated"},
 "capabilities":{"filesystem":{"mode":"read","blocked_paths":["secret.txt"]}}}' > "$HD/govern.json"
hard_run() { # NAME ENGINE BODY
    printf 'use file\nuse array\nmain {\n%s\n    print("AFTER")\n}\n' "$3" > "$HD/$1.naab"
    local a=("$NAAB" run); [ "$2" = tw ] && a+=(--tree-walk); a+=("$1.naab")
    OUT="$(cd "$HD" && timeout 30s "${a[@]}" 2>&1)"; RC=$?; OUT="${OUT//$'\r'/}"
}
# A HARD block needs enforce, where the tree-walker is refused: the VM measures
# this, and CT-TW pins the refusal.
source "$REPO/tests/helpers/tree_walk_refusal.sh"
hard_run "c-tw" tw '    try { let s = file.read("secret.txt") } catch (e) { print("CAUGHT") }'
if tw_refused "$RC" "$OUT" && ! grep -q 'CAUGHT\|^AFTER' <<<"$OUT"; then
    ok "CT-TW" "under an enforce config --tree-walk is refused before it runs"
else
    bad "CT-TW" "--tree-walk ran under an enforce config (rc=$RC)" "$(show)"
fi
for eng in vm; do
    hard_run "c-$eng" "$eng" '    try { let s = file.read("secret.txt") } catch (e) { print("CAUGHT") }'
    if [ "$RC" -eq 3 ] && ! grep -q 'CAUGHT\|^AFTER' <<<"$OUT"; then
        ok "CT-17c/$eng" "control: the blocked read called directly is HARD (rc=3, not caught)"
    else
        bad "CT-17c/$eng" "control: direct blocked read (rc=$RC)" "$(show)"
    fi
    for form in sort sorted; do
        hard_run "$form-$eng" "$eng" "    try { let r = array.$form([2, 1], fn(a, b) { let s = file.read(\"secret.txt\"); return 0 }) } catch (e) { print(\"CAUGHT \" + string(e)) }"
        if [ "$RC" -eq 3 ] && ! grep -q 'CAUGHT\|^AFTER' <<<"$OUT"; then
            ok "CT-17/$form/$eng" "a HARD block inside an array.$form comparator ends the run (rc=3, not caught)"
        else
            bad "CT-17/$form/$eng" "HARD block inside an array.$form comparator (rc=$RC)" "$(show)"
        fi
    done
done

# CT-18 -- V-UB-001's type check is unchanged. A native error is caught as a
# dict on both engines, so the arm looks for the message text.
for eng in vm tw; do
    run "ct18-$eng" "$eng" 'use array
main {
    try { let m = array.sort([2, 1], fn(x, y) { return "nope" }) } catch (e) { print("CAUGHT=" + string(e)) }
    print("AFTER")
}'
    if [ "$RC" -eq 0 ] && grep -q '^CAUGHT=.*comparator must return a number' <<<"$OUT" && grep -qx AFTER <<<"$OUT"; then
        ok "CT-18/$eng" "control (V-UB-001): a comparator returning a string is still refused"
    else
        bad "CT-18/$eng" "V-UB-001 type check (rc=$RC)" "$(show)"
    fi
done

# CT-20 -- contract golden tests on the same VM after a throwing one.
CD="$T/contract"; mkdir -p "$CD"
printf '%s\n' '{"version":"4.0","mode":"enforce","security":{"sandbox_level":"elevated"},
 "contracts":{"level":"advisory","functions":{
   "a_thrower":{"must_produce":[{"args":[1],"expect":2}]},
   "b_wrong":{"must_produce":[{"args":[1],"expect":99}]}}}}' > "$CD/govern.json"
printf '%s\n' 'fn a_thrower(x) { throw "contract-boom" }' 'fn b_wrong(x) { return x * 2 }' 'main { print("RAN") }' > "$CD/c.naab"
# the tree-walker beside an audit copy (it is refused under enforce)
mkdir -p "$CD/tw"; sed 's/"mode":"enforce"/"mode":"audit"/' "$CD/govern.json" > "$CD/tw/govern.json"; cp "$CD/c.naab" "$CD/tw/"
for eng in vm tw; do
    a=("$NAAB" run); cdir="$CD"; [ "$eng" = tw ] && { a+=(--tree-walk); cdir="$CD/tw"; }; a+=(c.naab)
    OUT="$(cd "$cdir" && timeout 30s "${a[@]}" 2>&1)"; RC=$?; OUT="${OUT//$'\r'/}"
    # Under audit (the tree-walker) a finding prints its first line only, so
    # "Got: 2" is the VM's half; both engines must test b_wrong after the throw.
    got_ok=yes; [ "$eng" = vm ] && ! grep -q 'Got: 2' <<<"$OUT" && got_ok=no
    if [ "$RC" -eq 0 ] && grep -q "'a_thrower' threw an exception" <<<"$OUT" \
       && grep -q "'b_wrong' returned wrong value" <<<"$OUT" && [ "$got_ok" = yes ]; then
        ok "CT-20/$eng" "after a throwing golden test, the next function is tested and reported (Got: 2)"
    else
        bad "CT-20/$eng" "contract tests after a throwing one (rc=$RC)" "$(show)"
    fi
done

# CT-19 -- the reported case: a throwing agent tool.
# The agent stub hangs build-windows (tests/helpers/stub_platform.sh), so this
# arm alone is skipped there -- the other arms launch no stub and still run.
ON_WINDOWS=0
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) ON_WINDOWS=1 ;; esac
[ -n "${WINDIR:-}" ] && ON_WINDOWS=1
if [ "$ON_WINDOWS" -eq 0 ]; then
    ( cd "$T" && "$NAAB" --keygen k.pem >/dev/null 2>&1 && "$NAAB" --trust-key k.pem.pub >/dev/null 2>&1 )
    export NAAB_SIGNING_KEY="$T/k.pem"; export FAKE_KEY_A41=fake
fi
tool_run() { # NAME ENGINE TOOL_BODY
    local d="$T/tool-$1-$2"; mkdir -p "$d"
    printf '%s\n' '{"responses":[{"tool_calls":[{"name":"g","args":{"q":"1"}}],"input_tokens":10,"output_tokens":10},{"content":"done","input_tokens":10,"output_tokens":10}]}' > "$d/fx.json"
    OUT=""; RC=0
    if ! start_stub "$d/fx.json" "$d" >/dev/null 2>&1; then OUT="STUBFAIL"; return; fi
    cat > "$d/govern.json" <<EOF
{"mode":"enforce","security":{"sandbox_level":"elevated"},
 "agents":{"a":{"provider":"gemini","model":"stub","api_base":"http://127.0.0.1:$STUB_PORT",
   "api_key_env":"FAKE_KEY_A41","max_tokens":100,"max_turns":30,"system_prompt":"You review invoices.",
   "tools_enabled":true,"tools":["g"],"max_tool_loop_turns":5,"max_tool_calls_per_turn":5}}}
EOF
    (cd "$d" && "$NAAB" --sign-governance >/dev/null 2>&1) || true
    cat > "$d/t.naab" <<EOF
use agent
fn g(q) { $3 }
main {
  agent.register_tool("g", g, {"description":"d","parameters":{"q":{"type":"string","description":"q"}}})
  let h = agent.create("a")
  let r = agent.send(h, "check")
  let tr = r.get("tool_results")
  print("CONTENT=" + string(r.get("content")) + " TOOL_OK=" + string(tr[0].get("success")))
  let local = {"k": 7}
  print("K=" + string(local.get("k")))
}
EOF
    local a=("$NAAB" run); [ "$2" = tw ] && a+=(--tree-walk); a+=(t.naab)
    OUT="$(cd "$d" && timeout 60s "${a[@]}" 2>&1)"; RC=$?; OUT="${OUT//$'\r'/}"
    stop_stub 2>/dev/null
}
# VM only: the tree-walker does not execute agent tools at all (F36) -- a
# returning tool reads TOOL_OK=false there, so a tree-walker arm would pass
# for a reason that has nothing to do with this defect.
if [ "$ON_WINDOWS" -eq 1 ]; then
    skip "CT-19" "agent stub not launched on Windows/MSYS2 (it hangs the runner; covered by build-linux)"
fi
for eng in $( [ "$ON_WINDOWS" -eq 0 ] && echo vm ); do
    tool_run returns "$eng" 'return "ok"'
    if [ "$OUT" = STUBFAIL ]; then skip "CT-19c/$eng" "agent stub did not start"; continue; fi
    if [ "$RC" -eq 0 ] && grep -qx 'CONTENT=done TOOL_OK=true' <<<"$OUT" && grep -qx 'K=7' <<<"$OUT"; then
        ok "CT-19c/$eng" "control: a returning tool, the reply and the script's locals"
    else
        bad "CT-19c/$eng" "control: returning tool (rc=$RC)" "$(show)"
    fi
    tool_run throws "$eng" 'throw "tool-boom"'
    if [ "$RC" -eq 0 ] && grep -qx 'CONTENT=done TOOL_OK=false' <<<"$OUT" && grep -qx 'K=7' <<<"$OUT"; then
        ok "CT-19/$eng" "a throwing tool is a failed tool result; the script carries on"
    else
        bad "CT-19/$eng" "throwing tool (rc=$RC)" "$(show)"
    fi
done

echo ""
echo "callback throw: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
