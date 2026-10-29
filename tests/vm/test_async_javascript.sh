#!/usr/bin/env bash
# test_async_javascript.sh -- a <<javascript>> block inside a VM async fn (A61)
#
# THE DEFECT
#
# Every VM async fn call runs on its own thread, and every <<javascript>> block
# went to the ONE QuickJS runtime the registry's adapter made on the main
# thread. A QuickJS runtime records its creating thread's stack as its limit,
# so on any other thread every evaluation fails its stack check -- before even
# the error can be turned into text, which is why each such block printed
# "JavaScript evaluation failed: Unknown error" (observed on master 6055ac3c,
# and in a standalone QuickJS program: the same runtime evaluates on its own
# thread, fails on another, and works there after JS_UpdateStackTop or when
# made on that thread). A runtime is not safe to share between threads either.
# A call from any thread but the adapter's own now runs on a fresh executor
# made on that thread for that call, as the tree-walker's parallel pool does.
#
#   AJ-00   CONTROL: the same JavaScript block in main{} returns 3
#   AJ-01   a JavaScript block inside an async fn returns 3
#   AJ-02   eight concurrent async fns, each evaluating JavaScript with its
#           own bound argument, each return their own result
#   AJ-03   an async fn's JavaScript runs on its own runtime: a global main's
#           JavaScript set is not visible there (the tree-walker runs every
#           block on a fresh runtime too)
#   AJ-04   a JavaScript loop inside an async fn stops at --timeout: the first
#           block ran (its marker printed), the loop was interrupted, and the
#           run ended inside the bound
#   AJ-05   a runtime gate still stands in front of the new route:
#           limits.execution.polyglot_blocks 1, main's block runs and the async
#           fn's block is refused HARD (exit 3, its marker absent)
#   AJ-05c  CONTROL: with the limit at 2 the same program runs both blocks --
#           AJ-05's refusal is the limit's
#
# Kill table (each mutant rebuilt, suite re-run; observed 2026-10-28):
#   onThisThread() always uses the owner's executor (the pre-fix shape)
#                                -> AJ-01, AJ-02, AJ-03, AJ-04, AJ-05c red
#   onThisThread() uses the owner's executor after JS_UpdateStackTop
#     (shares one runtime across threads)    -> AJ-03 red
# The per-thread last-call record (getCapturedOutput/getLastExitCode) is not
# visible from a program; tests/unit/js_executor_test.cpp LastErrorIsPerThread
# measures it, and fails on the pre-fix adapter (another thread read the first
# thread's exit code and error text). Its two cross-thread cases fail there too.
# Red first: on master AJ-01..AJ-04 and AJ-05c fail (the async block never
# runs); AJ-00 and AJ-05 pass (the refusal is in front of the executor there
# too).

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust          # unsigned configs
W=$(mktemp -d "${TMPDIR:-/tmp}/naab_asyncjs_XXXXXX")
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
unm()  { UNM=$((UNM+1));  echo "  UNMEASURABLE [$1] $2"; }

BASE='"mode": "enforce", "security": {"sandbox_level": "elevated"}'

# run ID CONFIG PROGRAM [extra naab-lang args...] -> OUT, RC, SECS
run() {
    local d="$W/$1"; mkdir -p "$d"
    printf '%s\n' "$2" > "$d/govern.json"
    printf '%s' "$3" > "$d/p.naab"
    shift 3
    local t0=$SECONDS
    RC=0; OUT=$(cd "$d" && timeout 60s "$NAAB" run "$@" p.naab 2>&1) || RC=$?
    SECS=$((SECONDS - t0))
}
has() { grep -q -F -- "$1" <<<"$OUT"; }

echo "=== JavaScript inside a VM async fn (A61) ==="

run aj00 "{ $BASE }" $'main {\n    let v = <<javascript\n1 + 2\n>>\n    print("MAIN-JS=" + string(v))\n}\n'
if [ "$RC" -eq 0 ] && has "MAIN-JS=3"; then
    pass AJ-00 "CONTROL: a JavaScript block in main{} returns 3"
    JS_OK=1
else
    unm AJ-00 "JavaScript does not run in main{} here: rc=$RC $(head -c 300 <<<"$OUT" | tr '\n' ' ')"
    JS_OK=0
fi

if [ "$JS_OK" -eq 1 ]; then
    run aj01 "{ $BASE }" $'async fn work() {\n    let v = <<javascript\n1 + 2\n>>\n    return v\n}\nmain {\n    print("ASYNC-JS=" + string(await work()))\n}\n'
    if [ "$RC" -eq 0 ] && has "ASYNC-JS=3"; then
        pass AJ-01 "a JavaScript block inside an async fn returns 3"
    else
        fail AJ-01 "a JavaScript block inside an async fn should return 3" "rc=$RC $(head -c 300 <<<"$OUT" | tr '\n' ' ')"
    fi

    run aj02 "{ $BASE }" $'async fn work(n) {\n    let v = <<javascript[n]\nn * 10 + 1\n>>\n    return v\n}\nmain {\n    let fs = []\n    let i = 0\n    while i < 8 {\n        fs.push(work(i))\n        i = i + 1\n    }\n    let got = []\n    for f in fs {\n        got.push(string(await f))\n    }\n    print("GOT=" + got.join(","))\n}\n'
    if [ "$RC" -eq 0 ] && has "GOT=1,11,21,31,41,51,61,71"; then
        pass AJ-02 "eight concurrent async fns each get their own JavaScript result"
    else
        fail AJ-02 "eight concurrent async fns should return 1,11,...,71 in order" "rc=$RC $(grep -m1 'GOT=' <<<"$OUT")$(head -c 200 <<<"$OUT" | tr '\n' ' ')"
    fi

    run aj03 "{ $BASE }" $'async fn peek() {\n    let t = <<javascript\ntypeof globalThis.aj03Mark\n>>\n    return t\n}\nmain {\n    let m = <<javascript\nglobalThis.aj03Mark = 7\n>>\n    print("SET=" + string(m))\n    print("ASYNC-SEES=" + string(await peek()))\n}\n'
    if [ "$RC" -eq 0 ] && has "SET=7" && has "ASYNC-SEES=undefined"; then
        pass AJ-03 "an async fn's JavaScript runs on its own runtime (main's global is not there)"
    else
        fail AJ-03 "an async fn's JavaScript should not see main's runtime" "rc=$RC $(grep -E 'SET=|ASYNC-SEES=' <<<"$OUT" | tr '\n' ' ')$(head -c 200 <<<"$OUT" | tr '\n' ' ')"
    fi

    run aj04 "{ $BASE }" $'async fn spin() {\n    let s = <<javascript\n"AJ04-STARTED"\n>>\n    print(s)\n    let v = <<javascript\nlet i = 0\nwhile (true) { i = i + 1 }\ni\n>>\n    return v\n}\nmain {\n    print("END=" + string(await spin()))\n}\n' --timeout 3
    if [ "$RC" -ne 0 ] && has "AJ04-STARTED" && has "interrupted" && ! has "END=" && [ "$SECS" -lt 20 ]; then
        pass AJ-04 "a JavaScript loop inside an async fn stops at --timeout (${SECS}s)"
    else
        fail AJ-04 "the async fn's JavaScript loop should run and be interrupted at --timeout" "rc=$RC secs=$SECS $(head -c 300 <<<"$OUT" | tr '\n' ' ')"
    fi
else
    for a in AJ-01 AJ-02 AJ-03 AJ-04; do unm "$a" "AJ-00 found no working JavaScript executor"; done
fi

GATE_PROG=$'async fn work() {\n    let v = <<javascript\n"AJ05-ASYNC"\n>>\n    print(v)\n    return 1\n}\nmain {\n    let m = <<javascript\n"AJ05-MAIN"\n>>\n    print(m)\n    print(await work())\n}\n'
run aj05 "{ $BASE, \"limits\": {\"execution\": {\"polyglot_blocks\": 1}} }" "$GATE_PROG"
if [ "$RC" -eq 3 ] && has "AJ05-MAIN" && ! has "AJ05-ASYNC"; then
    pass AJ-05 "limits.execution.polyglot_blocks refuses the async fn's JavaScript block HARD"
else
    fail AJ-05 "the async fn's block should be refused at the block limit" "rc=$RC $(head -c 300 <<<"$OUT" | tr '\n' ' ')"
fi
run aj05c "{ $BASE, \"limits\": {\"execution\": {\"polyglot_blocks\": 2}} }" "$GATE_PROG"
if [ "$JS_OK" -eq 1 ]; then
    if [ "$RC" -eq 0 ] && has "AJ05-MAIN" && has "AJ05-ASYNC"; then
        pass AJ-05c "CONTROL: at a limit of 2 both blocks run"
    else
        fail AJ-05c "at a limit of 2 both blocks should run" "rc=$RC $(head -c 300 <<<"$OUT" | tr '\n' ' ')"
    fi
else
    unm AJ-05c "AJ-00 found no working JavaScript executor"
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed, $UNM unmeasurable ==="
[ "$FAIL" -eq 0 ] && [ "$UNM" -eq 0 ]
