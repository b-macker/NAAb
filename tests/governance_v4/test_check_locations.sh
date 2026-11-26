#!/usr/bin/env bash
# ============================================================
# test_check_locations.sh -- a refusal records the line it happened on (A171)
#
# THE DEFECT (observed on 58796deb, docs/open-investigations.md A171)
#
# A check result takes its file and line from one per-thread note. Two places
# set the note and never put it back: the output-contract check every function
# return runs (contract or not) and the polyglot-block check. The runtime gates
# -- filesystem mode and path, environment, network, a taint sink -- set none,
# so they recorded the last return's or last block's line, or 0 with neither.
# The refusal message named no line at all ("At: in function 'get'" inside a
# function). With telemetry.deduplicate_checks on, two refusals after one
# return shared that stale (rule, file, line) and telemetry kept one row.
#
# THE FIX: the engine asks the running VM for the current instruction's
# location whenever no explicit location is in force; every explicit location
# is scoped (ScopedCheckContext -- the unscoped setter is gone); the refusal
# message names the line; a live location is a runtime event, never collapsed.
#
# ARMS (each refusal arm reads the JSON report unless named otherwise)
#   CL-00  control: a polyglot refusal records its block's line (right before
#          the fix too -- proves the harness reads the reports at all)
#   CL-01  filesystem path refusal after a function return: the read's line
#   CL-01s ...the same in SARIF        CL-01t ...the same in telemetry
#   CL-02  env blocked_read            CL-03  network disabled
#   CL-04  filesystem mode none        CL-05  taint sink
#   CL-06  a read after a polyglot block: the read's line, not the block's
#   CL-07  inside a function: the read's line, and the message says
#          "At: line N, in function 'get'"
#   CL-08  in main: the message says "At: line N"
#   CL-09  inside an async fn (its own VM, its own thread)
#   CL-10  inside an array.map_fn callback (a nested VM run)
#   CL-11  dedup on: two refusals after one return are two telemetry rows
#   CL-11b dedup on: two refusals on ONE line (a loop) are two rows -- the
#          live-location flag's own arm
#   CL-11c control: dedup still collapses a repeated static site
#   CL-12  control: a contract result still records its return line (an
#          explicit location wins over the live one)
#   CL-13  a module compiled at run time keeps the module's file and line
#   CL-14  tree-walker (audit): the call's line
#   CL-15  a dangerous-call refusal at a NAAb stdlib call names the call's
#          line in its message when the argument was bound on the line before
#          (the line VM::stdlibCallGates() passes; A82 re-check, Bundle 19 PR 3)
#   CL-16  a behavioural-sequence pre-check refusal names the call's line (the
#          live location). The line the VM hands the pre-check itself goes only
#          into the event's `line`, which no report a suite reads carries
#          (BSD_MATCH has none): its `- 4` mutant survives, unobservable.
#
# KILL TABLE (mutants, each turns the named arms red; observed 2026-11-16,
# each reverted alone and rebuilt -- the first driver restored sources with an
# older mtime, so make kept every earlier mutant and all of them read alike)
#   M1 no live location (VM::run registers no provider)   CL-01..CL-11
#   M2 live location wins over an explicit one            CL-13
#   M3 dedup ignores the live-location flag               CL-11b (survived before CL-11b)
#   M4 formatError adds no "At: line N"                   CL-07m, CL-08
#   M5 tree-walker stdlib branch scopes no location       CL-14
#   M6 the contract check's location left unscoped        CL-01..CL-05, CL-08, CL-11
#   M7 stdlibCallGates' line read 3 instructions back     CL-15 (Bundle 19 PR 3; the
#      A82 offset, `- 4`; no arm caught it before CL-15)
#   (the BSD pre-check's own line read 3 instructions back: SURVIVES -- nothing
#    any suite reads carries it; see CL-16)
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"
_SYSTMP="${TMPDIR:-/tmp}"
TEST_TMP="${_SYSTMP}/naab-check-locations-$$"

PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo "  PASS [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo "  SKIP [$1] $2 (UNMEASURABLE)"; }

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
cleanup() { teardown_isolated_trust; rm -rf "${TEST_TMP:?}"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"

if [ ! -x "$NAAB" ]; then
    echo "naab-lang not built at $NAAB (UNMEASURABLE)"; exit 1
fi

echo "=== test_check_locations.sh (A171) ==="

# write_arm DIR CONFIG_BODY PROGRAM  -- a project with telemetry on.
write_arm() {
    mkdir -p "$1"
    cat > "$1/govern.json" <<EOF
{"version":"4.0","mode":"${MODE:-enforce}","security":{"sandbox_level":"elevated"},
 "telemetry":{"enabled":true,"output_file":"tele.jsonl"${DEDUP:-}},
 $2}
EOF
    printf '%s' "$3" > "$1/p.naab"
}

# run_arm DIR [extra args]  -- runs p.naab, writes out.txt (stdout), err.txt, RC.
run_arm() {
    local d="$1"; shift
    (cd "$d" && rm -f tele.jsonl r.json r.sarif &&
     SECRET_X=v VAR_ONE=1 VAR_TWO=2 timeout 60 "$NAAB" run p.naab "$@" \
        --governance-report r.json --governance-sarif r.sarif > out.txt 2> err.txt)
    RC=$?
}

# lines DIR RULE_PREFIX SOURCE -- the line of each non-passing row whose rule
# starts with RULE_PREFIX, from SOURCE = json | sarif | tele.
lines() {
    python3 - "$1" "$2" "$3" <<'EOF'
import json, sys, os
d, prefix, src = sys.argv[1:]
out = []
try:
    if src == "json":
        for r in json.load(open(os.path.join(d, "r.json")))["results"]:
            if not r["passed"] and r["rule"].startswith(prefix):
                out.append(str(r["line"]))
    elif src == "sarif":
        for run in json.load(open(os.path.join(d, "r.sarif")))["runs"]:
            for r in run["results"]:
                if r["ruleId"].startswith(prefix):
                    loc = r.get("locations", [{}])[0].get("physicalLocation", {})
                    out.append(str(loc.get("region", {}).get("startLine", 0)))
    else:
        for l in open(os.path.join(d, "tele.jsonl"), encoding="utf-8"):
            e = json.loads(l)
            if e.get("event_type") == "RuleViolation" and e.get("rule_name", "").startswith(prefix):
                out.append(str(e.get("line")))
except (OSError, ValueError, KeyError) as ex:
    print("UNREADABLE:" + type(ex).__name__); sys.exit()
print(" ".join(out))
EOF
}

# expect_lines ID DESC DIR RULE SOURCE WANT
expect_lines() {
    local got; got="$(lines "$3" "$4" "$5")"
    case "$got" in
        UNREADABLE:*) skip "$1" "$2: $5 output unreadable ($got, rc=$RC)" ;;
        "$6")          pass "$1" "$2 (line $got)" ;;
        "")            fail "$1" "$2: no failing $4 row (rc=$RC)" "$(head -3 "$3/out.txt" | tr '\n' ' ')" ;;
        *)             fail "$1" "$2: recorded line(s) '$got', expected '$6'" ;;
    esac
}

RET_FN=$'fn a() {\n    return 1\n}\n'          # lines 2-4 after one `use` line; return on line 3

# ---------- CL-00 control: polyglot refusal ----------
W="$TEST_TMP/cl00"
write_arm "$W" '"capabilities":{"shell":{"enabled":false}}' \
$'use file\n'"$RET_FN"$'main {\n    let r = a()\n    let pad = 0\n    let k = <<sh\necho hi\n>>\n}\n'
run_arm "$W"
expect_lines "CL-00" "control: a polyglot refusal records its block's line" "$W" "capabilities.shell" json 8

# ---------- CL-01: file path, JSON / SARIF / telemetry ----------
W="$TEST_TMP/cl01"
write_arm "$W" '"capabilities":{"filesystem":{"mode":"read","blocked_paths":["vault/"]}}' \
$'use file\n'"$RET_FN"$'main {\n    let r = a()\n    let pad = 0\n    let k = file.read("vault/x.txt")\n}\n'
mkdir -p "$W/vault"; echo s > "$W/vault/x.txt"
run_arm "$W"
expect_lines "CL-01"  "file path refusal after a return: the read's line (JSON)" "$W" "capabilities.filesystem.path" json 8
expect_lines "CL-01s" "...the same in SARIF" "$W" "capabilities.filesystem.path" sarif 8
expect_lines "CL-01t" "...the same in telemetry" "$W" "capabilities.filesystem.path" tele 8

# ---------- CL-02..CL-05: the other runtime gates ----------
W="$TEST_TMP/cl02"
write_arm "$W" '"capabilities":{"env_vars":{"blocked_read":["SECRET_X"]}}' \
$'use env\n'"$RET_FN"$'main {\n    let r = a()\n    let pad = 0\n    let k = env.get("SECRET_X")\n}\n'
run_arm "$W"
expect_lines "CL-02" "env blocked_read" "$W" "capabilities.env_vars" json 8

W="$TEST_TMP/cl03"
write_arm "$W" '"capabilities":{"network":{"enabled":false}}' \
$'use http\n'"$RET_FN"$'main {\n    let r = a()\n    let pad = 0\n    let k = http.get("https://example.com")\n}\n'
run_arm "$W"
expect_lines "CL-03" "network disabled" "$W" "capabilities.network" json 8

W="$TEST_TMP/cl04"
write_arm "$W" '"capabilities":{"filesystem":{"mode":"none"}}' \
$'use file\n'"$RET_FN"$'main {\n    let r = a()\n    let pad = 0\n    let k = file.read("x.txt")\n}\n'
run_arm "$W"
expect_lines "CL-04" "filesystem mode none" "$W" "capabilities.filesystem" json 8

W="$TEST_TMP/cl05"
write_arm "$W" '"taint_tracking":{"enabled":true,"level":"hard","sources":["env.get"],"sinks":["file.write"]},"capabilities":{"filesystem":{"mode":"write"}}' \
$'use env\nuse file\nfn a() {\n    return 1\n}\nmain {\n    let r = a()\n    let pad = 0\n    file.write("o.txt", env.get("HOME"))\n}\n'
run_arm "$W"
expect_lines "CL-05" "taint sink" "$W" "taint_tracking" json 9

# ---------- CL-06: after a polyglot block ----------
W="$TEST_TMP/cl06"
write_arm "$W" '"capabilities":{"filesystem":{"mode":"read","blocked_paths":["vault/"]}}' \
$'use file\nmain {\n    let v = <<python\n1 + 1\n>>\n    let pad = 0\n    let k = file.read("vault/x.txt")\n}\n'
mkdir -p "$W/vault"; echo s > "$W/vault/x.txt"
run_arm "$W"
expect_lines "CL-06" "a read after a polyglot block: the read's line" "$W" "capabilities.filesystem.path" json 7

# ---------- CL-07 / CL-08: the message names the line ----------
W="$TEST_TMP/cl07"
write_arm "$W" '"capabilities":{"filesystem":{"mode":"read","blocked_paths":["vault/"]}}' \
$'use file\nfn get() {\n    let q = 1\n    let k = file.read("vault/x.txt")\n    return k\n}\nmain {\n    let y = 0\n    let z = get()\n}\n'
mkdir -p "$W/vault"; echo s > "$W/vault/x.txt"
run_arm "$W"
expect_lines "CL-07" "inside a function: the read's line" "$W" "capabilities.filesystem.path" json 4
if grep -qF "At: line 4, in function 'get'" "$W/out.txt"; then
    pass "CL-07m" "the message says \"At: line 4, in function 'get'\""
else
    fail "CL-07m" "message lacks \"At: line 4, in function 'get'\" (rc=$RC)" "$(grep -m1 'At:' "$W/out.txt")"
fi
W="$TEST_TMP/cl01"   # CL-01's run: the refusal is in main on line 8
if grep -qE '^  At: line 8$' "$W/out.txt"; then
    pass "CL-08" "in main, the message says \"At: line 8\""
else
    fail "CL-08" "message lacks \"At: line 8\"" "$(grep -m1 'At:' "$W/out.txt" || echo 'no At: line')"
fi

# ---------- CL-09: inside an async fn ----------
W="$TEST_TMP/cl09"
write_arm "$W" '"capabilities":{"filesystem":{"mode":"read","blocked_paths":["vault/"]}}' \
$'use file\nasync fn reader() {\n    let q = 1\n    return file.read("vault/x.txt")\n}\nmain {\n    let r = await reader()\n}\n'
mkdir -p "$W/vault"; echo s > "$W/vault/x.txt"
run_arm "$W"
expect_lines "CL-09" "inside an async fn: the read's line" "$W" "capabilities.filesystem.path" json 4

# ---------- CL-10: inside a callback ----------
W="$TEST_TMP/cl10"
write_arm "$W" '"capabilities":{"filesystem":{"mode":"read","blocked_paths":["vault/"]}}' \
$'use file\nuse array\nmain {\n    let names = ["x.txt"]\n    let out = array.map_fn(names, fn(n) {\n        return file.read("vault/" + n)\n    })\n}\n'
mkdir -p "$W/vault"; echo s > "$W/vault/x.txt"
run_arm "$W"
expect_lines "CL-10" "inside an array.map_fn callback: the read's line" "$W" "capabilities.filesystem.path" json 6

# ---------- CL-11: dedup keeps two refusals; CL-11c static control ----------
W="$TEST_TMP/cl11"
DEDUP=',"deduplicate_checks":true' write_arm "$W" '"capabilities":{"env_vars":{"allowed_read":["HOME"]}}' \
$'use env\n'"$RET_FN"$'main {\n    let r = a()\n    let x = env.get("VAR_ONE")\n    let y = env.get("VAR_TWO")\n    print("done")\n}\n'
run_arm "$W" --governance-override
expect_lines "CL-11" "dedup on: two refusals after one return stay two telemetry rows" "$W" "capabilities.env_vars.allowed_read" tele "7 8"

# The same line twice (a loop): only the live-location flag keeps these apart --
# CL-11's two lines differ, so a dedup that ignored the flag still passed it
# (mutant L-M3 survived until this arm).
W="$TEST_TMP/cl11b"
DEDUP=',"deduplicate_checks":true' write_arm "$W" '"capabilities":{"env_vars":{"allowed_read":["HOME"]}}' \
$'use env\nmain {\n    let names = ["VAR_ONE", "VAR_TWO"]\n    for n in names {\n        let x = env.get(n)\n    }\n    print("done")\n}\n'
run_arm "$W" --governance-override
expect_lines "CL-11b" "dedup on: two refusals on one line (a loop) stay two telemetry rows" "$W" "capabilities.env_vars.allowed_read" tele "5 5"

W="$TEST_TMP/cl11c"
DEDUP=',"deduplicate_checks":true' write_arm "$W" '"restrictions":{"dangerous_calls":{"enabled":true,"level":"advisory"}}' \
$'main {\n    <<python\nx = eval("1+1")\ny = eval("2+2")\n>>\n    print("DONE")\n}\n'
run_arm "$W"
N="$(lines "$W" "restrictions.dangerous_calls" tele | wc -w)"
NJ="$(lines "$W" "restrictions.dangerous_calls" json | wc -w)"
if [ "$NJ" -ge 2 ] && [ "$N" -lt "$NJ" ]; then
    pass "CL-11c" "control: a repeated static site is still collapsed ($NJ results, $N telemetry rows)"
else
    fail "CL-11c" "static site not collapsed, or the fixture fired too few times ($NJ results, $N rows, rc=$RC)"
fi

# ---------- CL-12: a contract keeps its return line ----------
W="$TEST_TMP/cl12"
write_arm "$W" '"contracts":{"functions":{"h":{"return_not_null":true,"level":"hard"}}}' \
$'fn h(x) {\n    let y = x\n    return null\n}\nmain {\n    let pad = 0\n    let r = h(1)\n}\n'
run_arm "$W"
expect_lines "CL-12" "control: a contract result records its return line" "$W" "contracts" json 3

# ---------- CL-13: a module compiled at run time ----------
W="$TEST_TMP/cl13"
write_arm "$W" '"code_quality":{"no_placeholders":{"enabled":true,"level":"advisory"}}' \
$'import "mod.naab" as m\nmain {\n    let pad = 0\n    print(m.f())\n}\n'
printf 'export fn f() {\n    let q = 1\n    // TODO: finish\n    return q\n}\n' > "$W/mod.naab"
run_arm "$W"
GOT="$(python3 - "$W" <<'EOF'
import json, sys, os
try:
    rs = json.load(open(os.path.join(sys.argv[1], "r.json")))["results"]
except (OSError, ValueError, KeyError):
    print("UNREADABLE"); sys.exit()
print(" ".join(f"{os.path.basename(r['file'])}:{r['line']}" for r in rs
               if not r["passed"] and r["rule"] == "code_quality.no_placeholders"))
EOF
)"
if [ "$GOT" = "mod.naab:3" ]; then
    pass "CL-13" "a module's finding keeps the module's file and line ($GOT)"
elif [ "$GOT" = "UNREADABLE" ] || [ -z "$GOT" ]; then
    skip "CL-13" "no placeholder row from the module (rc=$RC; the fixture did not reach the check)"
else
    fail "CL-13" "module finding recorded at '$GOT', expected mod.naab:3"
fi

# ---------- CL-14: tree-walker under audit ----------
W="$TEST_TMP/cl14"
MODE=audit write_arm "$W" '"capabilities":{"env_vars":{"allowed_read":["HOME"]}}' \
$'use env\n'"$RET_FN"$'main {\n    let r = a()\n    let pad = 0\n    let k = env.get("VAR_ONE")\n}\n'
run_arm "$W" --tree-walk
expect_lines "CL-14" "tree-walker (audit): the call's line" "$W" "capabilities.env_vars.allowed_read" json 8

# ---------- CL-15: the line a stdlib call's gates are given (A82) ----------
# The message's "At: line N" comes from the line stdlibCallGates() passes to
# the dangerous-call check (the report row's line is the live location, which
# CL-01 covers). The argument is bound on the line before, so an offset that
# reads three instructions back lands there.
W="$TEST_TMP/cl15"
write_arm "$W" '"restrictions":{"dangerous_calls":{"level":"hard","blocklist_extra":["file.read"]}}' \
$'use file\n'"$RET_FN"$'main {\n    let p = "in.txt"\n    let k = file.read(p)\n}\n'
printf 'x\n' > "$W/in.txt"
run_arm "$W"
if [ "$RC" -eq 3 ] && grep -q 'At: line 7: naab block' "$W/out.txt" "$W/err.txt"; then
    pass "CL-15" "a dangerous-call refusal names the stdlib call's line (line 7)"
elif [ "$RC" -ne 3 ]; then
    fail "CL-15" "the call was not refused (rc=$RC) -- the fixture did not reach the check" "$(head -3 "$W/err.txt" | tr '\n' ' ')"
else
    fail "CL-15" "the refusal named another line" "$(grep -h 'At: line' "$W/out.txt" "$W/err.txt" | head -2 | tr '\n' ' ')"
fi

# ---------- CL-16: the line the BSD pre-check is given (A82) ----------
W="$TEST_TMP/cl16"
write_arm "$W" '"behavioral_sequences":{"enabled":true,"patterns":[{"name":"one_read","sequence":["file.read"],"level":"hard"}]}' \
$'use file\n'"$RET_FN"$'main {\n    let p = "in.txt"\n    let k = file.read(p)\n}\n'
printf 'x\n' > "$W/in.txt"
run_arm "$W"
if [ "$RC" -eq 3 ] && grep -q 'blocked before execution' "$W/out.txt" "$W/err.txt" && grep -qx '  At: line 7' "$W/out.txt" "$W/err.txt"; then
    pass "CL-16" "a sequence pre-check refusal names the stdlib call's line (line 7)"
elif [ "$RC" -ne 3 ]; then
    fail "CL-16" "the call was not refused (rc=$RC) -- the fixture did not reach the check" "$(head -3 "$W/err.txt" | tr '\n' ' ')"
else
    fail "CL-16" "the refusal named another line" "$(grep -h 'At: line' "$W/out.txt" "$W/err.txt" | head -2 | tr '\n' ' ')"
fi

echo
echo "=== $PASS_COUNT passed, $FAIL_COUNT failed, $SKIP_COUNT unmeasurable ==="
[ "$FAIL_COUNT" -eq 0 ] && [ "$SKIP_COUNT" -eq 0 ]
