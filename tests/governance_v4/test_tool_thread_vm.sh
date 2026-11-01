#!/usr/bin/env bash
# test_tool_thread_vm.sh -- a tool runs on the VM the agent call came from (A93)
#
# A tool registered with agent.register_tool() is NAAb code. Every tool used to
# run on the MAIN VM, through the callback main.cpp binds at startup, from
# whatever thread the tool loop ran on. Two routes put two threads on one VM:
#   - agent.batch / fan_out: each send runs on a pool worker, so two models that
#     both call a tool push frames onto the main VM's stack at once. Observed on
#     19cebb05 (5 of 5 runs, max_concurrent 2): the tools returned "Modulo
#     requires integers" from a loop over integers, and main's own batch result
#     read null afterwards (exit 1).
#   - an async fn's agent.send: the tool ran on main's stack while main was
#     executing its own code. Observed (5 runs): SIGSEGV twice, a wrong sum in
#     main once, main's `file` module read as null, "Dangling handle" in the tool.
# A tool now runs nested on the VM executing on its own thread, or, on a batch
# worker, on the dispatching VM lent to the batch and entered by one worker at
# a time (naab/thread_vm.h).
#
#   TV-00   CONTROL: a batch of two run one at a time (max_concurrent 1) gives
#           both tools the right sum -- the program and the arithmetic are right
#   TV-00b  CONTROL: in the two-at-once run the two sends' model calls were in
#           flight together (overlapping stub windows), so TV-01 measures the
#           concurrent case and not a batch that happened to run in sequence
#   TV-01   two-at-once batch, three runs: both tools give the right sum, both
#           results come back, and main's own values after the batch are intact
#   TV-02   an async fn's tool beside main's own loop: both sums right, both
#           calls recorded, exit 0
#   TV-03   a batched tool keeps the dispatcher's frames: a function granted only
#           AGENT_SEND dispatches the batch, and the tool's write is refused,
#           naming that function (a fresh VM per tool would drop the caller's
#           frame from the intersection and let the write through)
#   TV-03c  CONTROL: the same batch dispatched from main {} (no entry): the write lands
#   TV-04   an async fn's tool is attributed to the async fn: an async fn granted
#           only AGENT_SEND sends, and the tool's write is refused naming it
#   TV-04c  CONTROL: the async fn granted FS_WRITE too: the write lands
#
# Kill table (each mutant rebuilt and this suite re-run; observed 2026-11-01):
#   main.cpp's tool callback bound to the main VM again       -> TV-01, TV-02, TV-04
#   callToolOnThreadVM enters a lent VM without its lock      -> TV-01
#   agentBatch does not lend its VM (no LentVMScope)          -> TV-00, TV-01, TV-03, TV-03c
#   a fresh VM per tool (lent VM's globals, empty frames)     -> TV-03
# The loan's revocation and scopes are tests/unit/thread_vm_test.cpp (ThreadVm.*):
#   ~VMLoan does not revoke     -> ALoanIsRevokedWhenTheDispatcherLeaves, ARevokedLoanRefusesEntry
#   revocation not checked      -> ARevokedLoanRefusesEntry
#   lent entry without its lock -> TwoWorkersEnterALentVmOneAtATime (SIGSEGV)
# Red first: on master fc9b8099, TV-01 and TV-02 fail (as above) and TV-04 lets
# the write through; every control passes.

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
TEST_TMP=$(mktemp -d "${TMPDIR:-/tmp}/naab_toolvm_XXXXXX")

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -8 | sed 's/^/       | /'; }
unm()  { UNM=$((UNM+1));  echo "  UNMEASURABLE [$1] $2"; }

echo "=== A tool runs on the VM the agent call came from (A93) ==="
if [ ! -x "$NAAB" ]; then
    unm TV-00 "naab-lang not built -- every arm UNMEASURABLE, not a pass"
    echo "Results: $PASS passed, $FAIL failed, $UNM unmeasurable"; exit 1
fi

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
STUB_PORT=""
source "$SCRIPT_DIR/../helpers/stub_launch.sh"
cleanup() { stop_stub 2>/dev/null; teardown_isolated_trust; [ -n "${KEEP_TMP:-}" ] && { rm -rf "$KEEP_TMP"; cp -r "$TEST_TMP" "$KEEP_TMP"; }; rm -rf "$TEST_TMP"; }
trap cleanup EXIT

"$NAAB" --keygen "$TEST_TMP/key.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/key.pem.pub" >/dev/null 2>&1
export NAAB_SIGNING_KEY="$TEST_TMP/key.pem"
export FAKE_KEY_TOOLVM="fake-key-toolvm"

N=200000
SUM=$(python3 -c "print(sum(i % 7 for i in range($N)))")

# Each agent is routed by a token in its system prompt; its first reply is a
# call to `crunch`, held open so two sends overlap at the stub, and its second
# the final text. On build-windows (#354) TV-00b failed at a 300 and a 1200 ms
# hold; the windows its FAIL line carries showed why: each run's keys.log held
# three of its four requests, one of the two concurrent first calls missing. The
# two calls did overlap -- both second calls followed them -- but the stub's two
# threads appended their lines at once, and a Windows append is not atomic
# between file handles. The stub now serialises its log appends (A112;
# test_stub_routing.sh SR-09).
HOLD_MS=300
FIXTURE='{"routes": {
  "TOKEN-ALPHA": {"responses": [{"tool_calls": [{"name": "crunch", "args": {"tag": "alpha"}}], "hold_ms": '"$HOLD_MS"'}, {"content": "DONE-alpha"}]},
  "TOKEN-BRAVO": {"responses": [{"tool_calls": [{"name": "crunch", "args": {"tag": "bravo"}}], "hold_ms": '"$HOLD_MS"'}, {"content": "DONE-bravo"}]}
}, "responses": [{"content": "unrouted"}]}'

agent_block() {  # name token
    cat <<A
        "$1": {
            "provider": "gemini", "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": "FAKE_KEY_TOOLVM",
            "system_prompt": "$2",
            "max_tokens": 100, "max_turns": 20, "retry": {"max_attempts": 1},
            "tools_enabled": true, "tools": ["crunch"]
        }
A
}

CRUNCH="fn crunch(tag) {
    let acc = 0
    let i = 0
    while i < $N {
        acc = acc + (i % 7)
        i = i + 1
    }
    file.append(\"calls.log\", tag + \" \" + string(acc) + \"\\n\")
    return tag + \":\" + string(acc)
}"

# run_case ID MAX_CONCURRENT FUNCTIONS_JSON PROGRAM_BODY -> OUT, RC, WDIR
run_case() {
    WDIR="$TEST_TMP/$1"; mkdir -p "$WDIR"
    printf '%s\n' "$FIXTURE" > "$WDIR/fixture.json"
    if ! start_stub "$WDIR/fixture.json" "$WDIR"; then OUT="STUB-FAILED"; RC=99; return; fi
    local funcs=""
    [ -n "$3" ] && funcs="\"capabilities\": {\"functions\": $3},"
    cat > "$WDIR/govern.json" <<G
{
    "mode": "enforce",
    "security": {"sandbox_level": "elevated"},
    $funcs
    "agent_dispatch": {"max_concurrent": $2, "pool_size": $2},
    "agents": {
$(agent_block wa TOKEN-ALPHA),
$(agent_block wb TOKEN-BRAVO)
    }
}
G
    (cd "$WDIR" && "$NAAB" --sign-governance >/dev/null 2>&1) || true
    printf 'use agent\nuse file\n\n%s\n\n%s\n' "$CRUNCH" "$4" > "$WDIR/t.naab"
    RC=0; OUT=$(cd "$WDIR" && timeout 120s "$NAAB" t.naab 2>&1) || RC=$?
    printf '%s\n' "$OUT" > "$WDIR/out.txt"
    stop_stub
}
REG='    agent.register_tool("crunch", crunch, {
        "description": "crunch numbers",
        "parameters": {"tag": {"type": "string", "description": "tag"}}
    })'
BATCH_MAIN="main {
$REG
    let before = 12345
    let a = agent.create(\"wa\")
    let b = agent.create(\"wb\")
    let rs = agent.batch([a, b], [\"go alpha\", \"go bravo\"])
    for r in rs {
        print(\"RESULT \" + string(r.get(\"content\")))
    }
    let j = 0
    let s = 0
    while j < 1000 {
        s = s + j
        j = j + 1
    }
    print(\"AFTER \" + string(before) + \" \" + string(s))
}"
calls_ok() {  # tags... : calls.log holds exactly one right line per tag
    local t
    [ -f "$WDIR/calls.log" ] || return 1
    for t in "$@"; do
        [ "$(grep -c "^$t $SUM\$" "$WDIR/calls.log" 2>/dev/null)" = "1" ] || return 1
    done
    [ "$(wc -l < "$WDIR/calls.log" 2>/dev/null)" -eq "$#" ]
}
batch_ok() {
    [ "$RC" -eq 0 ] && grep -q '^RESULT DONE-alpha$' <<<"$OUT" && grep -q '^RESULT DONE-bravo$' <<<"$OUT" \
        && grep -q '^AFTER 12345 499500$' <<<"$OUT" && calls_ok alpha bravo
}
why() { echo "rc=$RC $(grep -E '^RESULT|^AFTER|^MAIN|^ASYNC|rror' <<<"$OUT" | head -4 | tr '\n' ' ') calls.log: $(tr '\n' ',' < "$WDIR/calls.log" 2>/dev/null)"; }
# overlap: the stub's first two requests (both sends' first model calls) were
# open at once. Prints every request's window (n:start-end, ms since stub start),
# so a failing control shows whether the calls ran one after the other.
overlap() {
    # Bytes on stdin, not a path: a native python3 under MSYS2 cannot open /tmp/...
    [ -f "$WDIR/keys.log" ] || { echo "no keys.log"; return 1; }
    python3 -c '
import sys
rows = []
for line in sys.stdin.buffer.read().decode("utf-8", "replace").splitlines():
    p = line.split()
    if len(p) >= 4:
        rows.append((int(p[0]), int(p[2]), int(p[3])))
rows.sort()
if len(rows) < 2:
    sys.exit(1)
(_, s1, e1), (_, s2, e2) = rows[0], rows[1]
print(" ".join("%d:%d-%d" % r for r in rows))
sys.exit(0 if s1 < e2 and s2 < e1 else 1)' < "$WDIR/keys.log"
}

# ---- TV-00: one at a time -----------------------------------------------------
run_case tv00 1 "" "$BATCH_MAIN"
if [ "$OUT" = "STUB-FAILED" ]; then unm TV-00 "the stub did not start"
elif batch_ok; then pass TV-00 "CONTROL: a batch of two run one at a time gives both tools the right sum ($SUM)"
else fail TV-00 "CONTROL: the one-at-a-time batch should give both tools the right sum" "$(why)"; fi

# ---- TV-01 / TV-00b: two at once, three runs -----------------------------------
ok=0; olap=0; last=""; windows=""
for k in 1 2 3; do
    run_case "tv01_$k" 2 "" "$BATCH_MAIN"
    [ "$OUT" = "STUB-FAILED" ] && continue
    batch_ok && ok=$((ok+1)) || last="$(why)"
    w="$(overlap)" && olap=$((olap+1))
    windows="$windows run $k: $w;"
done
if [ "$olap" -eq 3 ]; then pass TV-00b "CONTROL: the two sends' model calls were in flight together in 3 of 3 runs"
else fail TV-00b "CONTROL: the two sends should overlap at the stub (3 of 3), saw $olap -- TV-01 cannot measure concurrency; request windows, ms (hold $HOLD_MS):$windows"; fi
if [ "$ok" -eq 3 ]; then pass TV-01 "two batched tools at once: right sums, both results, main's values intact (3 of 3 runs)"
else fail TV-01 "two batched tools at once should both run correctly and leave main intact: $ok of 3" "$last"; fi

# ---- TV-02: an async fn's tool beside main's own loop --------------------------
ASYNC_MAIN="async fn worker(h) {
    let r = agent.send(h, \"go alpha\")
    return r.get(\"content\")
}

main {
$REG
    let a = agent.create(\"wa\")
    let f = worker(a)
    let mine = crunch(\"main\")
    print(\"MAIN \" + mine)
    let got = await f
    print(\"ASYNC \" + string(got))
}"
ok=0; last=""
for k in 1 2 3; do
    run_case "tv02_$k" 2 "" "$ASYNC_MAIN"
    [ "$OUT" = "STUB-FAILED" ] && continue
    if [ "$RC" -eq 0 ] && grep -q "^MAIN main:$SUM\$" <<<"$OUT" && grep -q '^ASYNC DONE-alpha$' <<<"$OUT" \
        && calls_ok main alpha; then ok=$((ok+1)); else last="$(why)"; fi
done
if [ "$ok" -eq 3 ]; then pass TV-02 "an async fn's tool runs beside main's loop: both sums right, exit 0 (3 of 3 runs)"
else fail TV-02 "an async fn's tool should not disturb main or itself: $ok of 3" "$last"; fi

# ---- TV-03 / TV-03c: a batched tool keeps the dispatcher's frames --------------
FUNCS_ORCH='{"level": "hard",
    "orchestrate": {"allowed_actions": ["AGENT_SEND"]},
    "crunch": {"allowed_actions": ["FS_WRITE", "FS_READ"]}}'
ORCH_MAIN="fn orchestrate(a, b) {
    return agent.batch([a, b], [\"go alpha\", \"go bravo\"])
}

main {
$REG
    let a = agent.create(\"wa\")
    let b = agent.create(\"wb\")
    let rs = orchestrate(a, b)
    print(\"DISPATCHED \" + string(len(rs)))
}"
run_case tv03 1 "$FUNCS_ORCH" "$ORCH_MAIN"
if [ "$OUT" = "STUB-FAILED" ]; then unm TV-03 "the stub did not start"
elif [ "$RC" -eq 3 ] && [ ! -s "$WDIR/calls.log" ] && grep -q "Narrowed by caller 'orchestrate'" <<<"$OUT"; then
    pass TV-03 "a batched tool's write is refused by the dispatching function's entry, which is named"
else fail TV-03 "the tool's write should be refused by orchestrate's entry (exit 3, nothing written)" "$(why)"; fi

MAIN_DISPATCH="main {
$REG
    let a = agent.create(\"wa\")
    let b = agent.create(\"wb\")
    let rs = agent.batch([a, b], [\"go alpha\", \"go bravo\"])
    print(\"DISPATCHED \" + string(len(rs)))
}"
run_case tv03c 1 "$FUNCS_ORCH" "$MAIN_DISPATCH"
if [ "$OUT" = "STUB-FAILED" ]; then unm TV-03c "the stub did not start"
elif [ "$RC" -eq 0 ] && calls_ok alpha bravo; then
    pass TV-03c "CONTROL: the same batch dispatched from main {} writes (the refusal is orchestrate's)"
else fail TV-03c "CONTROL: a batch dispatched from main {} should write" "$(why)"; fi

# ---- TV-04 / TV-04c: an async fn's tool is attributed to the async fn ----------
ASYNC_ONLY="async fn worker(h) {
    let r = agent.send(h, \"go alpha\")
    return r.get(\"content\")
}

main {
$REG
    let a = agent.create(\"wa\")
    let got = await worker(a)
    print(\"ASYNC \" + string(got))
}"
run_case tv04 1 '{"level": "hard", "worker": {"allowed_actions": ["AGENT_SEND"]},
    "crunch": {"allowed_actions": ["FS_WRITE", "FS_READ"]}}' "$ASYNC_ONLY"
if [ "$OUT" = "STUB-FAILED" ]; then unm TV-04 "the stub did not start"
elif [ "$RC" -eq 3 ] && [ ! -s "$WDIR/calls.log" ] && grep -q "Narrowed by caller 'worker'" <<<"$OUT"; then
    pass TV-04 "an async fn's tool is held to the async fn's entry: the write is refused, naming it"
else fail TV-04 "the async fn's tool write should be refused by its entry (exit 3, nothing written)" "$(why)"; fi
run_case tv04c 1 '{"level": "hard", "worker": {"allowed_actions": ["AGENT_SEND", "FS_WRITE", "FS_READ"]},
    "crunch": {"allowed_actions": ["FS_WRITE", "FS_READ"]}}' "$ASYNC_ONLY"
if [ "$OUT" = "STUB-FAILED" ]; then unm TV-04c "the stub did not start"
elif [ "$RC" -eq 0 ] && calls_ok alpha && grep -q '^ASYNC DONE-alpha$' <<<"$OUT"; then
    pass TV-04c "CONTROL: the async fn granted FS_WRITE: the tool writes"
else fail TV-04c "CONTROL: the async fn granted FS_WRITE should let the tool write" "$(why)"; fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $UNM unmeasurable"
[ "$FAIL" -eq 0 ] && [ "$UNM" -eq 0 ] || exit 1
exit 0
