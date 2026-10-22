#!/usr/bin/env bash
# ============================================================
# test_bsd_stdlib_events.sh -- which stdlib calls behavioural sequence
# detection SEES, on every VM route that calls a stdlib function
#
# BSD knows a stdlib call only through the VM's classification of its name.
# There were two hand-written copies of that classification in OP_CALL_METHOD
# (the pre-execution check and the post-call emission), and they disagreed;
# the zero-argument member route (`env.get_all` without parentheses) had none.
# Measured on 2e9c120d, one program per call, through the engine's own event
# stream (--event-stream-fd, the MCP ledger's feed):
#
#   - env.get_all() -- the SAME code branch as env.list -- and env.set (the
#     same branch as env.set_var) recorded nothing (docs/open-investigations.md
#     A22(a)).
#   - So did env.get_int/get_float/get_bool, env.delete_var, env.load_dotenv,
#     file.write_lines, io.read_file, io.write_file and csv.read/read_dict/
#     write/write_dict: each performs the action an event type names, with the
#     path or variable as its first argument, under a name neither list had.
#   - file.read_lines was in the pre-check list but not the emission list: it
#     was never recorded as a step, and when it COMPLETED a pattern that did not
#     block, the pre-check's one-shot "this action was already recorded" flag
#     (B9(a)) waited for an emit that never came and swallowed the next
#     unrelated event instead.
#   - process.getpid() recorded a process.exec event (a "process." prefix rule).
#   - `env.get_all` without parentheses is CALLED on the VM (the member route),
#     with no BSD event and no function-attribution sync, so a function whose
#     capabilities.functions entry grants no action read the whole environment.
#
#   E-*   the event each call records (event stream; controls: the names that
#         were already recorded, and names that must record nothing)
#   B-*   a hard pattern refuses the action through the alias, beside the
#         canonical name (control: the pattern is live) and the action alone
#         (control: the pattern does not refuse everything)
#   S-*   the consumed flag cannot outlive the call that set it
#   F-*   the member route keeps function attribution
#   G-*   the member route runs the module gates OP_CALL_METHOD runs (one helper)
#   P-*   process.getpid no longer completes a process.exec step; process.run does
#
# VM only: the tree-walker records no BSD event for a stdlib call at all (B11),
# and it is being phased out (docs/plan-mcp-service.md).
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
_SYSTMP="${TMPDIR:-/tmp}"
TEST_TMP="$(mktemp -d "${_SYSTMP}/bsd-stdlib-events-XXXXXX")"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT+1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2 (UNMEASURABLE)"; }

# Every config here is unsigned, and with any key in the trust store an
# unsigned govern.json is an INTEGRITY BLOCK: measured, a store holding one key
# turned this suite into 0 pass / 17 fail / 28 skip. Isolate it.
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
cleanup() { teardown_isolated_trust; rm -rf "${TEST_TMP:?}"; }
trap cleanup EXIT

if [ ! -x "$NAAB" ]; then
    echo "naab-lang not built at $NAAB -- UNMEASURABLE, not a pass"
    exit 1
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  BSD: which stdlib calls are seen, on every VM call route    |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

# One arm directory: a config with the given patterns, the fixture files the
# programs read, and the program. Built-in patterns are observe-only so that
# only the arm's own patterns can refuse anything.
new_arm() {  # $1=name  $2=patterns JSON array -> prints the directory
    local d="$TEST_TMP/$1"
    mkdir -p "$d"
    cat > "$d/govern.json" <<GEOF
{ "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "behavioral_sequences": { "enabled": true, "default_pattern_enforcement": "observe",
    "patterns": $2 } }
GEOF
    echo hello > "$d/in.txt"
    printf 'a,b\n1,2\n' > "$d/in.csv"
    echo "A22_DOT=1" > "$d/dot.env"
    echo "$d"
}

# Run PROGRAM in DIR. Sets RC and OUT.
run_in() {  # $1=dir  $2=program text
    printf '%s\n' "$2" > "$1/prog.naab"
    OUT="$(cd "$1" && timeout 60s "$NAAB" run prog.naab 2>&1)"
    RC=$?
}

# ── E: the event each call records ───────────────────────────────────────────
echo -e "${CYAN}--- E: the event each call records (event stream) ---${NC}"
EW="$(new_arm events '[]')"

# Sets EVENTS to the recorded events of one call as "type<detail> ...", or
# NONE, and ERAN to 1 when the program printed RAN (it ran to the end). Not
# run in a $( ) subshell: ERAN would be lost with it.
events_of() {  # $1=use line  $2=statement
    printf 'use %s\nmain {\n  %s\n  print("RAN")\n}\n' "$1" "$2" > "$EW/prog.naab"
    rm -f "$EW/ev.txt" "$EW/o1.txt" "$EW/o2.txt" "$EW/o5.txt" "$EW/o6.csv" "$EW/o7.csv"
    local out
    out="$(cd "$EW" && timeout 60s "$NAAB" run --event-stream-fd 3 prog.naab 3>ev.txt 2>&1)"
    ERAN=0; grep -q '^RAN' <<<"$out" && ERAN=1
    EVENTS="$(python3 -c '
import json, sys
evs = []
for line in sys.stdin.buffer.read().decode("utf-8", "replace").splitlines():
    line = line.strip()
    if not line:
        continue
    e = json.loads(line)
    evs.append(e["t"] + "<" + e["d"] + ">")
sys.stdout.buffer.write((" ".join(evs) if evs else "NONE").encode())
' < "$EW/ev.txt")"
}

# Viability: the stream must exist here (it does not on Windows) and must show
# a call that has always been recorded. Without this every NONE below is a
# broken probe, not a finding.
events_of env 'let x = env.get("HOME")'; VIABLE="$EVENTS"
if [ "$ERAN" != 1 ] || [ "${VIABLE#env.get<}" = "$VIABLE" ]; then
    skip "E-00" "event stream unusable here: got [$VIABLE] ran=$ERAN"
    E_OK=0
else
    pass "E-00" "control: the event stream shows env.get ($VIABLE)"
    E_OK=1
fi

# id | use | statement | expected type, or NONE | why
while IFS='|' read -r id use stmt want why; do
    [ -z "$id" ] && continue
    [ "$E_OK" = 1 ] || { skip "$id" "$why"; continue; }
    events_of "$use" "$stmt"; got="$EVENTS"
    if [ "$ERAN" != 1 ]; then
        fail "$id" "$why -- the program did not run to the end" "events=[$got]"
    elif [ "$want" = NONE ]; then
        if [ "$got" = NONE ]; then pass "$id" "$why: no event"
        else fail "$id" "$why: expected no event" "events=[$got]"; fi
    else
        if [ "${got#"$want"<}" != "$got" ] && [ "${got#* }" = "$got" ]; then
            pass "$id" "$why: one $want event ($got)"
        else
            fail "$id" "$why: expected one $want event" "events=[$got]"
        fi
    fi
done <<'CASES'
E-01c|env|let x = env.list()|env.get|control, recorded before: env.list
E-01|env|let x = env.get_all()|env.get|env.get_all, the same branch as env.list
E-02|env|let x = env.get_int("A22_NOPE", 1)|env.get|env.get_int reads a variable's value
E-03|env|let x = env.get_float("A22_NOPE", 1.0)|env.get|env.get_float reads a variable's value
E-04|env|let x = env.get_bool("A22_NOPE", true)|env.get|env.get_bool reads a variable's value
E-05c|env|env.set_var("A22_X", "1")|env.set_var|control, recorded before: env.set_var
E-05|env|env.set("A22_X", "1")|env.set_var|env.set, the same branch as env.set_var
E-06|env|env.delete_var("A22_X")|env.set_var|env.delete_var changes the environment
E-07|env|let x = env.load_dotenv("dot.env")|file.read|env.load_dotenv reads a file
E-08c|file|let x = file.read("in.txt")|file.read|control, recorded before: file.read
E-08|file|let x = file.read_lines("in.txt")|file.read|file.read_lines (pre-checked, never emitted)
E-09c|file|file.write("o1.txt", "x")|file.write|control, recorded before: file.write
E-09|file|file.write_lines("o2.txt", ["x"])|file.write|file.write_lines writes a file
E-10|io|let x = io.read_file("in.txt")|file.read|io.read_file reads a file
E-11|io|io.write_file("o5.txt", "x")|file.write|io.write_file writes a file
E-12|csv|let x = csv.read("in.csv")|file.read|csv.read reads a file
E-13|csv|let x = csv.read_dict("in.csv")|file.read|csv.read_dict reads a file
E-14|csv|csv.write("o6.csv", [["a"]])|file.write|csv.write writes a file
E-15|csv|csv.write_dict("o7.csv", [{"a": "1"}])|file.write|csv.write_dict writes a file
E-16|env|let x = env.get_all|env.get|env.get_all with no parentheses (the member route)
E-17c|process|let x = process.run("true", [])|process.exec|control, recorded before: process.run
E-17|process|let x = process.getpid()|NONE|process.getpid starts no process
E-18|env|let x = env.has("HOME")|NONE|env.has reads no value
E-19|file|let x = file.exists("in.txt")|NONE|file.exists reads no content
E-20|csv|let x = csv.parse("a,b")|NONE|csv.parse touches no file
CASES

# ── B: a hard pattern refuses the action through the alias ───────────────────
echo -e "${CYAN}--- B: a hard pattern refuses through the alias ---${NC}"
# expect_refused ID WHY: exit 3, the pattern named, the write never happened.
expect_refused() {
    if [ "$RC" = 3 ] && grep -q "behavioral_sequences\.$3" <<<"$OUT" && [ ! -e "$W/out.txt" ]; then
        pass "$1" "$2: refused (exit 3) and the write did not happen"
    else
        fail "$1" "$2: expected a refusal by $3" "rc=$RC out.txt=$([ -e "$W/out.txt" ] && echo written || echo absent) $(tail -2 <<<"$OUT" | tr '\n' ' ')"
    fi
}
expect_ran() {
    if [ "$RC" = 0 ] && [ -e "$W/out.txt" ] && grep -q '^RAN' <<<"$OUT"; then
        pass "$1" "$2: ran (exit 0) and wrote out.txt"
    else
        fail "$1" "$2: expected the program to run" "rc=$RC $(tail -2 <<<"$OUT" | tr '\n' ' ')"
    fi
}

PAT_ENV='[ { "name": "env_then_write", "level": "hard", "max_gap": 5, "sequence": ["env.get", "file.write"] } ]'
W="$(new_arm b_env "$PAT_ENV")"
run_in "$W" 'use env
use file
main { let x = env.get("HOME") file.write("out.txt", "x") print("RAN") }'
expect_refused "B-01c" "control: env.get then file.write" env_then_write
rm -f "$W/out.txt"
run_in "$W" 'use env
use file
main { let x = env.get_all() file.write("out.txt", "x") print("RAN") }'
expect_refused "B-01" "env.get_all() then file.write" env_then_write
rm -f "$W/out.txt"
run_in "$W" 'use env
use file
main { let x = env.get_all file.write("out.txt", "x") print("RAN") }'
expect_refused "B-02" "env.get_all (no parentheses) then file.write" env_then_write
rm -f "$W/out.txt"
run_in "$W" 'use file
main { file.write("out.txt", "x") print("RAN") }'
expect_ran "B-01s" "control: file.write alone"

PAT_SET='[ { "name": "set_then_write", "level": "hard", "max_gap": 5, "sequence": ["env.set_var", "file.write"] } ]'
W="$(new_arm b_set "$PAT_SET")"
run_in "$W" 'use env
use file
main { env.set_var("A22_X", "1") file.write("out.txt", "x") print("RAN") }'
expect_refused "B-03c" "control: env.set_var then file.write" set_then_write
rm -f "$W/out.txt"
run_in "$W" 'use env
use file
main { env.set("A22_X", "1") file.write("out.txt", "x") print("RAN") }'
expect_refused "B-03" "env.set then file.write" set_then_write

PAT_RW='[ { "name": "read_then_write", "level": "hard", "max_gap": 5, "sequence": ["file.read", "file.write"] } ]'
W="$(new_arm b_rw "$PAT_RW")"
run_in "$W" 'use file
main { let x = file.read("in.txt") file.write("out.txt", x) print("RAN") }'
expect_refused "B-04c" "control: file.read then file.write" read_then_write
rm -f "$W/out.txt"
run_in "$W" 'use io
use file
main { let x = io.read_file("in.txt") file.write("out.txt", x) print("RAN") }'
expect_refused "B-04" "io.read_file then file.write" read_then_write
rm -f "$W/out.txt"
run_in "$W" 'use file
use io
main { let x = file.read("in.txt") io.write_file("out.txt", x) print("RAN") }'
expect_refused "B-05" "file.read then io.write_file" read_then_write

# ── S: the consumed flag cannot outlive its call ─────────────────────────────
# `adv` is an ADVISORY pattern whose last step is pre-checked: when the
# pre-check finds it complete, it records the action and lets it through,
# marking it consumed so the post-call emit does not record it twice. `hard`
# then needs an event the program produces NEXT. If the consumed mark outlives
# the call that set it, it swallows that event and `hard` never completes.
echo -e "${CYAN}--- S: the consumed flag cannot outlive its call ---${NC}"
PAT_S='[ { "name": "adv", "level": "advisory", "max_gap": 5, "sequence": ["env.get", "file.read"] },
         { "name": "hard", "level": "hard", "max_gap": 5, "sequence": ["decode", "file.write"] } ]'
W="$(new_arm s_pair "$PAT_S")"
run_in "$W" 'use env
use file
use crypto
main { let a = env.get("HOME") let x = file.read("in.txt") let d = crypto.base64_decode("eA==") file.write("out.txt", d) print("RAN") }'
expect_refused "S-01c" "control: adv completes on file.read, then decode + write" hard
rm -f "$W/out.txt"
run_in "$W" 'use env
use file
use crypto
main { let a = env.get("HOME") let x = file.read_lines("in.txt") let d = crypto.base64_decode("eA==") file.write("out.txt", d) print("RAN") }'
expect_refused "S-01" "adv completes on file.read_lines, then decode + write" hard
rm -f "$W/out.txt"
run_in "$W" 'use env
use file
use crypto
main {
  let a = env.get("HOME")
  try { let x = file.read("missing.txt") } catch (e) { print("READ_FAILED") }
  let d = crypto.base64_decode("eA==")
  file.write("out.txt", d)
  print("RAN")
}'
if grep -q READ_FAILED <<<"$OUT"; then
    expect_refused "S-02" "adv completes on a file.read that throws, then decode + write" hard
else
    fail "S-02" "the failing read did not fail -- the arm does not test its subject" "$(tail -2 <<<"$OUT" | tr '\n' ' ')"
fi

# The same, where the swallowed event has the SAME type as the consumed one:
# agent.send is pre-checked by the VM and recorded by the agent module itself,
# so a send that throws before its own record leaves the mark set, and the next
# AGENT_SEND event -- a tool re-registration -- was the one dropped.
PAT_A='[ { "name": "adv", "level": "advisory", "max_gap": 5, "sequence": ["env.get", "agent.send"] },
         { "name": "hard", "level": "hard", "max_gap": 5, "sequence": ["agent.send:*reregistration*", "file.write"] } ]'
W="$(new_arm s_agent "$PAT_A")"
run_in "$W" 'use agent
use file
fn t(x) { return x }
main {
  agent.register_tool("tt", t, {"description": "d"})
  agent.register_tool("tt", t, {"description": "d"})
  file.write("out.txt", "x")
  print("RAN")
}'
expect_refused "S-03c" "control: a tool re-registration then file.write" hard
rm -f "$W/out.txt"
run_in "$W" 'use agent
use env
use file
fn t(x) { return x }
main {
  let a = env.get("HOME")
  try { let r = agent.send({}, "x") } catch (e) { print("SEND_FAILED") }
  agent.register_tool("tt", t, {"description": "d"})
  agent.register_tool("tt", t, {"description": "d"})
  file.write("out.txt", "x")
  print("RAN")
}'
if grep -q SEND_FAILED <<<"$OUT"; then
    expect_refused "S-03" "adv completes on an agent.send that throws, then re-registration + write" hard
else
    fail "S-03" "the failing send did not fail -- the arm does not test its subject" "$(tail -2 <<<"$OUT" | tr '\n' ' ')"
fi

# ── F: the member route keeps function attribution ───────────────────────────
echo -e "${CYAN}--- F: the member route keeps function attribution ---${NC}"
FW="$TEST_TMP/f_attr"; mkdir -p "$FW"
cat > "$FW/govern.json" <<'GEOF'
{ "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "capabilities": { "functions": { "level": "hard",
    "harvest": { "allowed_actions": [] },
    "reader": { "allowed_actions": ["ENV_READ"] } } } }
GEOF
fattr() {  # $1=id  $2=function  $3=expression  $4=expect (refused|ran)  $5=why
    printf 'use env\nfn %s() { let d = %s return len(d) }\nmain { print("N=" + string(%s())) }\n' "$2" "$3" "$2" > "$FW/prog.naab"
    OUT="$(cd "$FW" && timeout 60s "$NAAB" run prog.naab 2>&1)"; RC=$?
    if [ "$4" = refused ]; then
        if [ "$RC" = 3 ] && grep -q "capabilities.functions.$2" <<<"$OUT" && ! grep -q '^N=' <<<"$OUT"; then
            pass "$1" "$5: refused, naming $2's entry"
        else
            fail "$1" "$5: expected a refusal naming $2" "rc=$RC $(grep -m1 -E '^N=|Error' <<<"$OUT")"
        fi
    else
        if [ "$RC" = 0 ] && grep -q '^N=' <<<"$OUT"; then pass "$1" "$5: ran ($(grep -m1 '^N=' <<<"$OUT"))"
        else fail "$1" "$5: expected it to run" "rc=$RC $(tail -2 <<<"$OUT" | tr '\n' ' ')"; fi
    fi
}
fattr "F-01c" harvest 'env.get_all()' refused "control: env.get_all() in a function granted nothing"
fattr "F-01"  harvest 'env.get_all'   refused "env.get_all (no parentheses) in a function granted nothing"
fattr "F-01s" reader  'env.get_all'   ran     "control: env.get_all (no parentheses) in a function granted ENV_READ"

# ── G: the member route runs the module gates ────────────────────────────────
# The filesystem MODE gate needs no argument, so a zero-argument call reaches it:
# `env.load_dotenv` without parentheses reads `.env`. Before the route shared
# OP_CALL_METHOD's gates (VM::stdlibCallGates), `filesystem.mode: "none"` stopped
# it only in the sandbox, as an error the program caught and ran on from (exit
# 0); the same call with parentheses is a HARD block. The file was read in
# neither case -- the defect is the tier.
echo -e "${CYAN}--- G: the member route runs the module gates ---${NC}"
gate_arm() {  # $1=id  $2=fs mode  $3=expression  $4=expect (hard|loaded)  $5=why
    local d="$TEST_TMP/g_$1"; mkdir -p "$d"
    printf '{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" },\n  "capabilities": { "filesystem": { "mode": "%s" } } }\n' "$2" > "$d/govern.json"
    echo "G_DOT_TOKEN=dot_loaded" > "$d/.env"
    printf 'use env\nuse file\nmain {\n  try {\n    let r = %s\n  } catch (e) {\n    print("CAUGHT")\n  }\n  print("TOKEN=" + (env.get("G_DOT_TOKEN") ?? "none"))\n}\n' "$3" > "$d/prog.naab"
    OUT="$(cd "$d" && timeout 60s "$NAAB" run prog.naab 2>&1)"; RC=$?
    if [ "$4" = hard ]; then
        if [ "$RC" = 3 ] && ! grep -q '^CAUGHT' <<<"$OUT" && ! grep -q 'TOKEN=dot_loaded' <<<"$OUT"; then
            pass "$1" "$5: HARD block, not caught, nothing loaded"
        else
            fail "$1" "$5: expected a HARD block" "rc=$RC caught=$(grep -c '^CAUGHT' <<<"$OUT") $(grep -m1 -E '^TOKEN=' <<<"$OUT")"
        fi
    else
        if [ "$RC" = 0 ] && grep -q 'TOKEN=dot_loaded' <<<"$OUT"; then pass "$1" "$5: loaded"
        else fail "$1" "$5: expected .env to load" "rc=$RC $(tail -2 <<<"$OUT" | tr '\n' ' ')"; fi
    fi
}
gate_arm "G-01c" none 'env.load_dotenv()' hard   "control: env.load_dotenv() under filesystem.mode none"
gate_arm "G-01"  none 'env.load_dotenv'   hard   "env.load_dotenv (no parentheses) under filesystem.mode none"
gate_arm "G-01s" read 'env.load_dotenv'   loaded "control: env.load_dotenv (no parentheses) where reads are allowed"
gate_arm "G-02c" read 'file.write()'      hard   "control: file.write() under filesystem.mode read"
gate_arm "G-02"  read 'file.write'        hard   "file.write (no parentheses) under filesystem.mode read"

# ── P: process.getpid is not a process start ─────────────────────────────────
echo -e "${CYAN}--- P: process.getpid is not a process start ---${NC}"
PAT_P='[ { "name": "env_then_exec", "level": "hard", "max_gap": 5, "sequence": ["env.get", "process.exec"] } ]'
W="$(new_arm p_exec "$PAT_P")"
run_in "$W" 'use process
use file
main { let r = process.run("true", []) file.write("out.txt", "x") print("RAN") }'
if [ "$RC" != 0 ] || [ ! -e "$W/out.txt" ]; then
    skip "P-01c" "process.run(\"true\") does not run here: rc=$RC"
    skip "P-01"  "needs P-01c's executor"
else
    rm -f "$W/out.txt"
    run_in "$W" 'use env
use process
use file
main { let a = env.get("HOME") let r = process.run("true", []) file.write("out.txt", "x") print("RAN") }'
    expect_refused "P-01c" "control: env.get then process.run" env_then_exec
    rm -f "$W/out.txt"
    run_in "$W" 'use env
use process
use file
main { let a = env.get("HOME") let p = process.getpid() file.write("out.txt", "x") print("RAN") }'
    expect_ran "P-01" "env.get then process.getpid"
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "  Passed: ${GREEN}$PASS_COUNT${NC}  Failed: ${RED}$FAIL_COUNT${NC}  Skipped: ${YELLOW}$SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then echo -e "${RED}FAILURES:${NC}$FAILURES"; exit 1; fi
echo -e "${GREEN}ALL PASSED${NC}"
exit 0
