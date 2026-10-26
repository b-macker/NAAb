#!/usr/bin/env bash
# ============================================================
# test_vm_async_governance.sh — a VM `async fn` ran with no VM-side governance
#
# THE DEFECT
#
# VM::callValue() runs every `async fn` call on its own thread in a fresh
# `VM async_vm`. 5ce3c20c gave that VM the engine (setGovernance); 4f65b2b0
# (2026-04-19, "VM async segfault fix") removed it on purpose -- "shared
# governance/module_resolver mutated concurrently" -- and #269 later put back
# only the THREAD-LOCAL engine pointer, for the embedded-Python audit hook. So
# every check the VM guards with its own `governance_` member was skipped
# inside an async fn on the default engine: the stdlib pre-dispatch filesystem
# gate, the polyglot block checks, the block count. Checks reached through the
# thread-local engine (env.get's checkEnvVarRead) still ran.
#
# Measured at dd7744fd, each config refusing the same call in main{} (exit 3):
# file.read of a blocked_paths file inside an async fn printed the file, exit 0;
# a <<python>> os.system under restrictions.code_injection ran and wrote its
# marker, exit 0; limits.execution.polyglot_blocks 2 let three blocks run when
# one was in an async fn. The tree-walker refused all three (its async path
# builds an engine of its own and restores the parent's counters, V-GOV-004).
#
# THE FIX
#
# The async VM gets the caller's engine again (setGovernance), and the engine
# state those checks change from several threads at once is made safe for it.
#
#   AG-00  CALIBRATION: an async fn that makes no governed call runs on the
#          VM -- without it every refusal below could be the async call
#          itself failing. The tree-walker is refused under enforce (Bundle 2)
#   AG-01..03  blocked_paths, code_injection, polyglot_blocks: refused inside an
#          async fn on the VM (the defect), with the same call refused in main
#          on the VM (the config works), the tree-walker refused under enforce
#          (Bundle 2), and RUN under the permit config (the
#          refusal is the gate's, not the program's)
#   AG-04  env.get of a blocked_read variable inside an async fn: refused on
#          both before and after the fix -- it reaches the thread-local engine,
#          which #269 already propagated. A control that the split is what the
#          defect was, not a general async failure
#   AG-05  the block count is SHARED across concurrent async fns: limit 6,
#          six concurrent async fns with one <<sqlite>> block each run (exit
#          0); a seventh is refused (exit 3); control: seven run with no limit.
#          SQLite runs in-process on every build, so the arm measures the count
#          and not an executor. A lost increment is a race this arm would catch
#          only by chance; the count is atomic by construction and the TSan run
#          is the evidence for that, not this arm
#   AG-05p the same with <<python>>, behind a viability probe (seven
#          concurrent Python async fns with no limit must run). Windows builds
#          run Python as a separate process, and it failed there at random:
#          concurrent blocks took the same temp script name, and one block's
#          cleanup deleted another's script (fixed in GenericSubprocessExecutor;
#          tests/robustness/test_compile_tempdir_cleanup.sh TD-C). Where the
#          probe fails the arm reports UNMEASURABLE with the error it saw
#   AG-06  a tainted argument reaching a sink is checked in EVERY concurrent
#          async fn. The VM used to mark a fixed label ("argument 1 of
#          'file.write()'") in the engine's shared taint set, check it, and
#          clear it: three lock acquisitions on a name every thread uses, so
#          one thread's clear landed between another's mark and check and that
#          write reached its sink unchecked. Measured before the fix, advisory
#          level, 8 async fns x 20 writes: 155-160 of 160 violations recorded,
#          all 160 in only 4 of 19 runs; ThreadSanitizer saw nothing, since
#          every step held the lock. Controls: main{} and ONE async fn record
#          all 160 (the counting works, and async alone loses nothing). Five
#          runs, so a regression passes by luck about 0.21^5 of the time
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../helpers/tree_walk_refusal.sh"

# Every govern.json below is unsigned; with a key in the ambient trust store an
# unsigned config is an INTEGRITY BLOCK at exit 3, the code this suite reads as
# "the gate fired". Repoint the store.
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust

_SYSTMP="${TMPDIR:-/tmp}"
TEST_TMP="${_SYSTMP}/naab-vm-async-gov-$$"
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0
pass() { PASS_COUNT=$((PASS_COUNT+1)); echo "  PASS [$1] $2"; }
# The detail goes on the FAIL line itself: run-all-tests.sh's summary keeps only
# PASS/FAIL/Result lines, and a detail on its own line never reached a CI log.
fail() { FAIL_COUNT=$((FAIL_COUNT+1)); echo "  FAIL [$1] $2${3:+ -- $3}"; }
skip() { SKIP_COUNT=$((SKIP_COUNT+1)); echo "  SKIP [$1] UNMEASURABLE: $2"; }
cleanup() { rm -rf "$TEST_TMP"; teardown_isolated_trust; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"
W="$TEST_TMP/w"

echo "=== test_vm_async_governance.sh ==="

if [ ! -x "$NAAB" ]; then
    skip "AG-ALL" "naab-lang not built"; echo "Result: 0 passed, 0 failed, 1 skipped"; exit 0
fi

reset_workspace() {
    rm -rf "$W"; mkdir -p "$W/vault"
    printf 'VAULT_SECRET_7731\n' > "$W/vault/secret.txt"
}

# $1=config $2=program $3=engine flag -> "<rc>|<ran|blocked>|<marker file present: y|n>"
run() {
    reset_workspace
    printf '%s\n' "$1" > "$W/govern.json"
    printf '%s\n' "$2" > "$W/t.naab"
    local o rc m=n
    o=$(cd "$W" && AG_BLOCKED_VAR=VALUE_THAT_MUST_NOT_PRINT timeout 60s "$NAAB" run t.naab ${3:-} 2>&1); rc=$?
    printf '%s\n' "$o" > "$TEST_TMP/last.out"
    [ -f "$W/MARK" ] && m=y
    if grep -q "MARKER" <<<"$o"; then echo "$rc|ran|$m"; else echo "$rc|blocked|$m"; fi
}

# The first line of the last run's output that says what went wrong, for a
# FAIL or SKIP line (run() is called in $( ), so it leaves its output in a file).
why() {
    grep -v '^\[governance\]' "$TEST_TMP/last.out" 2>/dev/null \
        | grep -m1 -iE "error|gone|can't|failed|denied" | cut -c1-160
}

BASE='"mode": "enforce", "security": { "sandbox_level": "elevated" }'
PERMIT="{ $BASE, \"capabilities\": { \"filesystem\": { \"mode\": \"write\" } } }"

# $1=id $2=label $3=config $4=async program $5=main-thread program
assert_async_gate() {
    local id="$1" label="$2" cfg="$3" prog="$4" mainprog="$5"
    local a t m p
    local t_refused=n
    m=$(run "$cfg" "$mainprog" "")
    a=$(run "$cfg" "$prog" "")
    # The tree-walker used to be a control here (the other engine refuses the
    # async call). Under enforce it is refused before anything runs (Bundle 2).
    t=$(run "$cfg" "$prog" "--tree-walk")
    grep -qF -- "$TW_REFUSAL_MARK" "$TEST_TMP/last.out" && t_refused=y
    p=$(run "$PERMIT" "$prog" "")
    if [ "$m" != "3|blocked|n" ]; then
        fail "$id" "$label" "CONTROL: the call in main{} on the VM was not refused ($m) -- the config does not exercise the gate"
    elif [ "$t" != "4|blocked|n" ] || [ "$t_refused" != y ]; then
        fail "$id" "$label" "the tree-walker should be refused under enforce ($t)"
    elif [ "$(cut -d'|' -f1,2 <<<"$p")" != "0|ran" ]; then
        fail "$id" "$label" "PERMIT CONTROL: the async fn did not run without the restriction ($p)"
    elif [ "$a" != "3|blocked|n" ]; then
        fail "$id" "$label" "the VM ran it inside an async fn ($a; expected 3|blocked|n)"
    else
        pass "$id" "$label"
    fi
}

# ---- AG-00 calibration ---------------------------------------------------------
CAL='async fn work() {
    return "x"
}
main {
    let f = work()
    let r = await f
    print("MARKER " + r)
}'
c_vm=$(run "{ $BASE }" "$CAL" "")
c_tw=$(run "{ $BASE }" "$CAL" "--tree-walk")
grep -qF -- "$TW_REFUSAL_MARK" "$TEST_TMP/last.out" || c_tw="$c_tw|not-the-refusal"
if [ "$c_vm" = "0|ran|n" ] && [ "$c_tw" = "4|blocked|n" ]; then
    pass "AG-00" "an async fn with no governed call runs on the VM (the tree-walker is refused under enforce)"
else
    fail "AG-00" "calibration failed (vm=$c_vm tree-walk=$c_tw) -- nothing below can be read"
    echo "Result: $PASS_COUNT passed, $FAIL_COUNT failed, $SKIP_COUNT skipped"; exit 1
fi

# ---- AG-01 blocked_paths ---------------------------------------------------------
FS_CFG="{ $BASE, \"capabilities\": { \"filesystem\": { \"mode\": \"write\", \"blocked_paths\": [\"vault/\"] } } }"
assert_async_gate AG-01 "blocked_paths holds inside an async fn" "$FS_CFG" \
'use file
async fn work() {
    return file.read("vault/secret.txt")
}
main {
    let f = work()
    let r = await f
    print("MARKER " + r)
}' \
'use file
main {
    print("MARKER " + file.read("vault/secret.txt"))
}'

# ---- AG-02 polyglot block checks -------------------------------------------------
INJ_CFG="{ $BASE, \"capabilities\": { \"filesystem\": { \"mode\": \"write\" } },
  \"restrictions\": { \"code_injection\": { \"enabled\": true, \"block_command_injection\": true } } }"
assert_async_gate AG-02 "restrictions.code_injection holds for a <<python>> block inside an async fn" "$INJ_CFG" \
'async fn work() {
    let a = <<python
import os
os.system("touch MARK")
1
>>
    return a
}
main {
    let f = work()
    let r = await f
    print("MARKER " + string(r))
}' \
'main {
    let a = <<python
import os
os.system("touch MARK")
1
>>
    print("MARKER " + string(a))
}'

# ---- AG-03 polyglot block count --------------------------------------------------
CNT_CFG="{ $BASE, \"capabilities\": { \"filesystem\": { \"mode\": \"write\" } },
  \"limits\": { \"execution\": { \"polyglot_blocks\": 2 } } }"
assert_async_gate AG-03 "limits.execution.polyglot_blocks counts a block inside an async fn" "$CNT_CFG" \
'async fn work() {
    let a = <<python
3
>>
    return a
}
main {
    let m1 = <<python
1
>>
    let m2 = <<python
2
>>
    let f = work()
    let r = await f
    print("MARKER " + string(r))
}' \
'main {
    let m1 = <<python
1
>>
    let m2 = <<python
2
>>
    let m3 = <<python
3
>>
    print("MARKER " + string(m3))
}'

# ---- AG-04 the thread-local route already held -----------------------------------
ENV_CFG="{ $BASE, \"capabilities\": { \"env_vars\": { \"blocked_read\": [\"AG_BLOCKED_VAR\"] } } }"
ENV_PROG='use env
async fn work() {
    return env.get("AG_BLOCKED_VAR")
}
main {
    let f = work()
    let r = await f
    print("MARKER " + string(r))
}'
e_vm=$(run "$ENV_CFG" "$ENV_PROG" "")
e_ok=$(run "{ $BASE }" "$ENV_PROG" "")
if [ "$e_vm" = "3|blocked|n" ] && [ "$(cut -d'|' -f1,2 <<<"$e_ok")" = "0|ran" ]; then
    pass "AG-04" "env.get inside an async fn is refused on the VM (thread-local route), and runs unrestricted"
else
    fail "AG-04" "thread-local env route inside an async fn" "restricted=$e_vm unrestricted=$e_ok"
fi

# ---- AG-05 one shared count across concurrent async fns --------------------------
mk_conc() {  # $1 = number of concurrent async fns, $2 = block (default sqlite)
    local n="$1" i
    local block
    if [ "${2:-sqlite}" = python ]; then block='<<python\n1\n>>'; else block='<<sqlite\nSELECT 1 AS v\n>>'; fi
    printf "async fn work(i) {\n    let a = $block\n    return i\n}\nmain {\n    let fs = []\n"
    for i in $(seq 1 "$n"); do printf '    fs.push(work(%s))\n' "$i"; done
    printf '    let total = 0\n    for f in fs {\n        total = total + (await f)\n    }\n    print("MARKER " + string(total))\n}\n'
}
CONC_CFG="{ $BASE, \"limits\": { \"execution\": { \"polyglot_blocks\": 6 } } }"
# $1 = id, $2 = block language
assert_shared_count() {
    local id="$1" lang="$2" free k6 k7 free_why k6_why k7_why
    free=$(run "{ $BASE }" "$(mk_conc 7 "$lang")" ""); free_why=$(why)
    k6=$(run "$CONC_CFG" "$(mk_conc 6 "$lang")" ""); k6_why=$(why)
    k7=$(run "$CONC_CFG" "$(mk_conc 7 "$lang")" ""); k7_why=$(why)
    if [ "$(cut -d'|' -f1,2 <<<"$free")" != "0|ran" ]; then
        fail "$id" "shared block count ($lang)" "CONTROL: seven async fns did not run with no limit ($free: $free_why)"
    elif [ "$(cut -d'|' -f1,2 <<<"$k6")" = "0|ran" ] && [ "$(cut -d'|' -f1,2 <<<"$k7")" = "3|blocked" ]; then
        pass "$id" "six concurrent async fns ($lang) fit a limit of six; a seventh is refused"
    else
        fail "$id" "shared block count across concurrent async fns ($lang)" "six=$k6 ($k6_why) seven=$k7 ($k7_why)"
    fi
}
assert_shared_count AG-05 sqlite

# ---- AG-05p the same count with Python blocks, where they can run concurrently -----
# The probe is the arm's own no-limit run (seven): it first asked for six, so a
# build that ran six and not seven passed the probe and failed the control.
probe=$(run "{ $BASE }" "$(mk_conc 7 python)" ""); probe_why=$(why)
if [ "$(cut -d'|' -f1,2 <<<"$probe")" != "0|ran" ]; then
    skip "AG-05p" "seven concurrent <<python>> async fns did not run with no limit on this build ($probe: $probe_why), so the Python variant cannot be read; AG-05 measures the count"
else
    assert_shared_count AG-05p python
fi

# ---- AG-06 every concurrent tainted write is checked -----------------------------
TAINT_CFG="{ $BASE, \"capabilities\": { \"filesystem\": { \"mode\": \"write\" } },
  \"taint_tracking\": { \"enabled\": true, \"level\": \"advisory\", \"sources\": [\"env.get\"], \"sinks\": [\"file.write\"] },
  \"telemetry\": { \"enabled\": true, \"output_file\": \"tel.jsonl\", \"deduplicate_checks\": false } }"
mk_taint() {  # $1 = async fns, $2 = writes each; $1 = 0 writes from main{}
    if [ "$1" -eq 0 ]; then
        printf 'use file\nuse env\nmain {\n    for k in 0..%s {\n        let h = env.get("AG_BLOCKED_VAR")\n        file.write("out.txt", h)\n    }\n    print("MARKER done")\n}\n' "$2"
        return
    fi
    printf 'use file\nuse env\nasync fn work(i) {\n    for k in 0..%s {\n        let h = env.get("AG_BLOCKED_VAR")\n        file.write("out_" + string(i) + ".txt", h)\n    }\n    return 1\n}\n' "$2"
    printf 'main {\n    let fs = []\n    for i in 0..%s {\n        fs.push(work(i))\n    }\n    let n = 0\n    for f in fs {\n        n = n + (await f)\n    }\n    print("MARKER " + string(n))\n}\n' "$1"
}
# $1 = program -> "<rc>|<violations recorded>"
taint_count() {
    run "$TAINT_CFG" "$1" "" >/dev/null
    local n=0
    [ -f "$W/tel.jsonl" ] && n=$(grep -c 'taint_tracking.sink_violation' "$W/tel.jsonl")
    printf '%s\n' "$n"
}
c_main=$(taint_count "$(mk_taint 0 160)")
c_one=$(taint_count "$(mk_taint 1 160)")
if [ "$c_main" != "160" ] || [ "$c_one" != "160" ]; then
    fail "AG-06" "every concurrent tainted write is checked" \
        "CONTROL: main{}=$c_main one-async=$c_one recorded, expected 160 each -- the count cannot be read"
else
    counts=""; short=0
    for r in 1 2 3 4 5; do
        c=$(taint_count "$(mk_taint 8 20)"); counts="$counts $c"
        [ "$c" = "160" ] || short=1
    done
    if [ "$short" -eq 0 ]; then
        pass "AG-06" "8 concurrent async fns x 20 tainted writes: all 160 checked in 5 of 5 runs"
    else
        fail "AG-06" "concurrent tainted writes reached the sink unchecked" "recorded per run:$counts (of 160)"
    fi
fi

echo "Result: $PASS_COUNT passed, $FAIL_COUNT failed, $SKIP_COUNT skipped"
[ "$FAIL_COUNT" -eq 0 ]
