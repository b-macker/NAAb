#!/usr/bin/env bash
# test_async_throw.sh -- a value thrown in a VM async fn reaches the await's catch (A59(a))
#
# THE DEFECT
#
# A VM async fn runs in its own VM on its own thread. A NAAb `throw` it did not
# catch left that VM as VMThrownValue, crossed the future as an exception, and
# OP_AWAIT turned every exception into "Await error: ... Cause: ..." through
# runtimeError -- so a catch around the await got a message dict, never the
# thrown value, where the tree-walker hands over the value itself (observed on
# master 6055ac3c: `throw "async-boom"` caught as {"message": "Await error: ...
# async-boom ..."}). The worker now copies the thrown value while its VM still
# exists (as it does a returned value), and OP_AWAIT rethrows it with its taint,
# copied into the awaiting thread, for run() to deliver to the catch.
#
#   AT-00  CONTROL: an async fn that returns hands the await its value
#   AT-01  a string thrown in an async fn is caught around the await as that
#          string
#   AT-02  a thrown dict arrives as the dict, nested values intact
#   AT-03  a tainted value thrown across the async boundary is still flagged
#          at the sink (written as string(e)); AT-03c CONTROL: an untainted
#          one is not. On master this was LAUNDERING: the catch got a fresh,
#          untainted message dict whose text held the tainted value, and
#          writing it passed with no violation (observed: an env.get value
#          reached file.write, exit 0)
#   AT-04  CONTROL: uncaught, the run fails with the thrown text and the await
#          site, and stops there
#   AT-05  CONTROL: a runtime error in an async fn (not a throw) still arrives
#          as a message dict
#   AT-06  six concurrent async fns, each throwing its own index, each caught
#          as that index
#   AT-07  the tree-walker gives AT-01/AT-02's program the same output (beside
#          an audit copy of the config: it is refused under enforce)
#   AT-08  CONTROL: a HARD block inside an async fn stays uncatchable around
#          the await (exit 3 at run time, after main started; the catch is
#          not entered)
#   AT-09  a closure RETURNED from an async fn can be called after the await.
#          The call ran on copies of the function tree that only the worker
#          owned, freed with the future, so the closure dangled: a crash on
#          master before this work, through the return path it never touched
#   AT-10  a closure THROWN out of an async fn -- built in a second async fn
#          it awaited, and wrapped in another closure -- can be called in the
#          catch. Delivering thrown values (AT-01) opened this route to the
#          same dangling copies, so the copies are now owned by the closures
#          made from them (VMClosure::function_owner)
#
# Kill table (each mutant rebuilt, suite re-run; observed 2026-10-28):
#   OP_AWAIT without its VMThrownValue catch (the pre-fix delivery)
#                               -> AT-01, AT-02, AT-03, AT-06, AT-07, AT-10 red
#   OP_AWAIT drops the taint flag                          -> AT-03 red
#   OP_CLOSURE does not inherit function_owner             -> AT-09, AT-10 red
#   the async call's closure gets no function_owner        -> AT-09, AT-10 red
# Red first (every hunk reverted, i.e. master): AT-01, AT-02, AT-03, AT-06,
# AT-07, AT-09 and AT-10 fail; the controls pass.

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust          # unsigned configs
W=$(mktemp -d "${TMPDIR:-/tmp}/naab_asyncthrow_XXXXXX")
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }

ENFORCE='{"mode":"enforce","security":{"sandbox_level":"elevated"}}'

# run ID CONFIG PROGRAM [--tree-walk] -> OUT, RC
run() {
    local d="$W/$1"; mkdir -p "$d"
    printf '%s\n' "$2" > "$d/govern.json"
    printf '%s' "$3" > "$d/p.naab"
    local a=("$NAAB" run); [ "${4:-}" = "--tree-walk" ] && a+=(--tree-walk); a+=(p.naab)
    RC=0; OUT=$(cd "$d" && A59_SRC=value timeout 60s "${a[@]}" 2>&1) || RC=$?
    OUT="${OUT//$'\r'/}"
}
has() { grep -q -F -- "$1" <<<"$OUT"; }
brief() { head -c 400 <<<"$OUT" | tr '\n' ' '; }

echo "=== A value thrown in a VM async fn (A59(a)) ==="

run at00 "$ENFORCE" $'async fn work() { return {"v": 41} }\nmain {\n    let r = await work()\n    print("AWAITED=" + string(r["v"] + 1))\n}\n'
if [ "$RC" -eq 0 ] && has "AWAITED=42"; then
    pass AT-00 "CONTROL: a returning async fn hands the await its value"
else
    fail AT-00 "a returning async fn should hand the await 42" "rc=$RC $(brief)"
fi

THROW_PROG=$'async fn work(kind) {\n    if kind == "str" { throw "async-boom" }\n    throw {"code": 7, "items": [1, 2]}\n}\nmain {\n    try {\n        await work("str")\n        print("NO-THROW")\n    } catch (e) {\n        print("STR type=" + type(e) + " value=" + string(e))\n    }\n    try {\n        await work("dict")\n    } catch (e) {\n        print("DICT type=" + type(e) + " code=" + string(e["code"]) + " item=" + string(e["items"][1]))\n    }\n    print("AFTER")\n}\n'
run at01 "$ENFORCE" "$THROW_PROG"
VM_OUT="$OUT"
if [ "$RC" -eq 0 ] && has "STR type=string value=async-boom"; then
    pass AT-01 "a thrown string is caught around the await as that string"
else
    fail AT-01 "the catch should receive the thrown string" "rc=$RC $(brief)"
fi
if [ "$RC" -eq 0 ] && has "DICT type=dict code=7 item=2" && has "AFTER"; then
    pass AT-02 "a thrown dict arrives as the dict, nested values intact"
else
    fail AT-02 "the catch should receive the thrown dict" "rc=$RC $(brief)"
fi

TAINT_CFG='{"version":"4.0","mode":"enforce","security":{"sandbox_level":"elevated"},
 "taint_tracking":{"enabled":true,"level":"advisory","sources":["env.get"],"sinks":["file.write"],"sanitizers":["sanitize_"]}}'
flagged() { grep -qiE 'sink_violation|Taint tracking violation' <<<"$OUT"; }
run at03 "$TAINT_CFG" $'use env\nuse file\nasync fn leak() {\n    let s = env.get("A59_SRC")\n    throw s\n}\nmain {\n    try { await leak() } catch (e) { file.write("out.txt", string(e)) }\n    print("DONE")\n}\n'
if [ "$RC" -eq 0 ] && flagged && has "DONE"; then
    pass AT-03 "a tainted value thrown across the async boundary is flagged at the sink"
else
    fail AT-03 "taint should survive the async throw" "rc=$RC $(brief)"
fi
run at03c "$TAINT_CFG" $'use env\nuse file\nasync fn clean() {\n    throw "literal"\n}\nmain {\n    try { await clean() } catch (e) { file.write("out.txt", string(e)) }\n    print("DONE")\n}\n'
if [ "$RC" -eq 0 ] && ! flagged && has "DONE"; then
    pass AT-03c "CONTROL: an untainted value thrown the same way is not flagged"
else
    fail AT-03c "an untainted async throw should not be flagged" "rc=$RC $(brief)"
fi

run at04 "$ENFORCE" $'async fn work() { throw "async-uncaught" }\nmain {\n    let r = await work()\n    print("NOT-REACHED")\n}\n'
if [ "$RC" -ne 0 ] && [ "$RC" -ne 3 ] && has "Await error" && has "async-uncaught" && has "p.naab:3" && ! has "NOT-REACHED"; then
    pass AT-04 "CONTROL: uncaught, the run fails with the thrown text and the await site"
else
    fail AT-04 "an uncaught async throw should end the run naming it and the await" "rc=$RC $(brief)"
fi

run at05 "$ENFORCE" $'async fn work() {\n    let d = {"a": 1}\n    return d["missing"]["deeper"]\n}\nmain {\n    try { await work() } catch (e) { print("RUNTIME type=" + type(e) + " has_message=" + string(e.has("message"))) }\n}\n'
if [ "$RC" -eq 0 ] && has "RUNTIME type=dict has_message=true"; then
    pass AT-05 "CONTROL: a runtime error in an async fn still arrives as a message dict"
else
    fail AT-05 "a runtime error should still arrive as a message dict" "rc=$RC $(brief)"
fi

run at06 "$ENFORCE" $'async fn work(n) { throw n * 10 }\nmain {\n    let fs = []\n    let i = 0\n    while i < 6 {\n        fs.push(work(i))\n        i = i + 1\n    }\n    let got = []\n    for f in fs {\n        try { await f } catch (e) { got.push(string(e)) }\n    }\n    print("GOT=" + got.join(","))\n}\n'
if [ "$RC" -eq 0 ] && has "GOT=0,10,20,30,40,50"; then
    pass AT-06 "six concurrent async throws are each caught as their own value"
else
    fail AT-06 "each concurrent async throw should arrive as its own value" "rc=$RC $(brief)"
fi

run at07 '{"mode":"audit","security":{"sandbox_level":"elevated"}}' "$THROW_PROG" --tree-walk
TW_LINES=$(grep -E '^(STR|DICT|AFTER|NO-THROW)' <<<"$OUT")
VM_LINES=$(grep -E '^(STR|DICT|AFTER|NO-THROW)' <<<"$VM_OUT")
if [ "$RC" -eq 0 ] && [ -n "$TW_LINES" ] && [ "$TW_LINES" = "$VM_LINES" ]; then
    pass AT-07 "the tree-walker gives the same output as the VM"
else
    fail AT-07 "VM and tree-walker should agree" "tw: $(tr '\n' '|' <<<"$TW_LINES") vm: $(tr '\n' '|' <<<"$VM_LINES")"
fi

HARD_CFG='{"mode":"enforce","security":{"sandbox_level":"elevated"},"capabilities":{"filesystem":{"mode":"read","blocked_paths":["vault/"]}}}'
mkdir -p "$W/at08/vault"; printf 'SECRET-AT08\n' > "$W/at08/vault/s.txt"
run at08 "$HARD_CFG" $'use file\nasync fn work() { return file.read("vault/s.txt") }\nmain {\n    print("AT08-STARTED")\n    try {\n        let r = await work()\n        print("READ=" + r)\n    } catch (e) {\n        print("CAUGHT")\n    }\n    print("AFTER")\n}\n'
if [ "$RC" -eq 3 ] && has "AT08-STARTED" && has "File path blocked" && ! has "CAUGHT" && ! has "SECRET-AT08" && ! has "AFTER"; then
    pass AT-08 "CONTROL: a HARD block inside an async fn stays uncatchable around the await"
else
    fail AT-08 "a HARD block inside an async fn must not be caught" "rc=$RC $(brief)"
fi

run at09 "$ENFORCE" $'async fn mk(k) {\n    return fn(x) { return x * k }\n}\nmain {\n    let f = await mk(6)\n    print("RETURNED=" + string(f(2)))\n    let g = await mk(3)\n    print("AGAIN=" + string(f(5) + g(5)))\n}\n'
if [ "$RC" -eq 0 ] && has "RETURNED=12" && has "AGAIN=45"; then
    pass AT-09 "a closure returned from an async fn can be called after the await"
else
    fail AT-09 "a returned closure should be callable" "rc=$RC $(brief)"
fi

run at10 "$ENFORCE" $'async fn inner(k) {\n    return [fn(x) { return x * k }, "tag"]\n}\nasync fn outer() {\n    let pair = await inner(7)\n    let f = pair[0]\n    throw {"f": f, "g": fn(y) { return f(y) + 1 }}\n}\nmain {\n    try { await outer() } catch (e) {\n        let f = e["f"]\n        let g = e["g"]\n        print("NEST f=" + string(f(3)) + " g=" + string(g(3)))\n    }\n}\n'
if [ "$RC" -eq 0 ] && has "NEST f=21 g=22"; then
    pass AT-10 "a closure thrown out of nested async fns can be called in the catch"
else
    fail AT-10 "a thrown closure should be callable in the catch" "rc=$RC $(brief)"
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed, $UNM unmeasurable ==="
[ "$FAIL" -eq 0 ] && [ "$UNM" -eq 0 ]
