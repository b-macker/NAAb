#!/usr/bin/env bash
# ============================================================
# Limits and switches that refuse an action and that no test noticed
# (setting liveness, step 2 of the owner's order of 2026-11-13; A167)
# ============================================================
# docs/settings-liveness.md lists settings a suite loads and none notices when
# removed. This suite takes the ones that REFUSE AN ACTION and whose default
# is off, unlimited or weaker, and pins each as it behaves today: the setting
# set, the action is refused; the setting absent (or at its looser value), the
# same program runs. It changes no engine behaviour.
#
# Every pair runs its CONTROL first. A control that does not run means the
# platform cannot run the fixture (no Python on build-windows, no stub), so
# both arms are UNMEASURABLE, never a failure of the setting.
#
#   codegen (refusals are catchable errors, exit 1 -- A88, the owner's call)
#   UL-01/01c  codegen.max_code_size_bytes     a longer code string is refused
#   UL-02/02c  codegen.max_cumulative_calls    the call past the cap is refused
#   UL-03/03c  codegen.max_cumulative_code_bytes  the call crossing it is refused
#   limits
#   UL-04/04c  limits.execution.call_depth     recursion past it is a HARD block
#   UL-05/05c  languages.per_language.python.max_lines      HARD block
#   UL-06/06c  languages.per_language.javascript.max_lines  HARD block
#   restrictions (both on by default at soft)
#   UL-07/07c  restrictions.obfuscation.level hard: --governance-override does
#              not pass it; at the default (soft) the override does
#   UL-07d     CONTROL: with obfuscation.enabled false the block runs
#   UL-08/08c  restrictions.vcs_secret_extraction.level hard, same shape
#   UL-08d     CONTROL: with vcs_secret_extraction.enabled false it runs
#   UL-09a/b/c restrictions.code_injection.block_dynamic_code_gen /
#              block_command_injection / block_sql_injection_patterns: with one
#              switch false its own pattern runs, and (UL-09x) another switch's
#              pattern is still refused -- one switch does not open the rest
#   agent (tests/helpers/agent_stub.py; the observable is what the stub got)
#   UL-10/10c  exposure_tracking.max_autonomous_actions  the send past it makes
#              no provider call
#   UL-11/11c  exposure_tracking.max_unique_agents       a second agent is refused
#              (at agent.create, a catchable error, exit 1 -- A168)
#   UL-12/12c  agents.<n>.tool_result_max_chars          the model receives the
#              truncated result, not the whole one
#   UL-13/13c  agents.<n>.tool_result_max_total_chars    the result crossing the
#              total is withheld from the model
#
# KILL TABLE. The mutant for each arm is the setting removed as the config
# loads, on the test build (docs/settings-liveness.md):
#   NAAB=build-mut/naab-lang NAAB_DROP_SETTING=<setting> bash <this suite>
# With no removal the suite passes on that build (32 of 32). Each row is the
# setting removed and the arm that FAILED (observed 2026-11-14, 007e8dd4):
#   codegen.max_code_size_bytes ..................... UL-01
#   codegen.max_cumulative_calls .................... UL-02
#   codegen.max_cumulative_code_bytes ............... UL-03
#   limits.execution.call_depth ..................... UL-04
#   languages.per_language.python.max_lines ......... UL-05
#   languages.per_language.javascript.max_lines ..... UL-06
#   restrictions.obfuscation.level .................. UL-07
#   restrictions.obfuscation.enabled ................ UL-07d
#   restrictions.vcs_secret_extraction.level ........ UL-08
#   restrictions.vcs_secret_extraction.enabled ...... UL-08d
#   restrictions.code_injection.block_dynamic_code_gen ....... UL-09a
#   restrictions.code_injection.block_command_injection ...... UL-09b
#   restrictions.code_injection.block_sql_injection_patterns . UL-09c
#   exposure_tracking.max_autonomous_actions ........ UL-10
#   exposure_tracking.max_unique_agents ............. UL-11
#   agents.*.tool_result_max_chars .................. UL-12
#   agents.*.tool_result_max_total_chars ............ UL-13
#
# NOT HERE, and why: limits.code.max_lines_per_block, limits.timeout.per_block
# and languages.per_language.<lang>.timeout refuse nothing (observed: a 4 s
# block ran under a 1 s limit; A2 has them, step 3 decides them);
# codegen.max_nesting_depth and max_cumulative_calls_per_agent need codegen
# called from inside codegen or from a tool (no fixture yet);
# exposure_tracking.max_pipeline_depth needs a pipeline fixture.
# ============================================================
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
source "$SCRIPT_DIR/../helpers/stub_platform.sh" 2>/dev/null
source "$SCRIPT_DIR/../helpers/trust_setup.sh"; setup_isolated_trust
source "$SCRIPT_DIR/../helpers/stub_launch.sh"

PASS=0; FAIL=0; SKIP=0
ok(){ PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad(){ FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
skip(){ SKIP=$((SKIP+1)); echo "  SKIP [$1] UNMEASURABLE: $2"; }

T="$(mktemp -d)"
cleanup(){ stop_stub 2>/dev/null; teardown_isolated_trust; [ -n "${KEEP_TMP:-}" ] || rm -rf "$T"; }
trap cleanup EXIT
[ -x "$NAAB" ] || { echo "naab-lang not built"; exit 1; }
export FAKE_KEY_UL=fake

BASE='"version":"4.0","mode":"enforce","security":{"sandbox_level":"elevated"}'

# run DIR CONFIG_BODY PROGRAM [FLAG] -> OUT RC CFG_OK
run() {
    local d="$T/$1"; mkdir -p "$d"
    printf '{%s%s}\n' "$BASE" "$2" > "$d/govern.json"
    CFG_OK=0; python3 -c 'import json,sys; json.load(sys.stdin)' < "$d/govern.json" && CFG_OK=1
    printf '%s\n' "$3" > "$d/p.naab"
    local a=("$NAAB" run p.naab); [ -n "${4:-}" ] && a+=("$4")
    OUT="$(cd "$d" && timeout 90s "${a[@]}" 2>&1)"; RC=$?
}
has(){ grep -qF -- "$1" <<<"$OUT"; }

# pair ID "what" CTL_CFG SET_CFG PROGRAM SET_RC SET_TEXT [FLAG]
# control: must exit 0 and print DONE. set: must exit SET_RC, print SET_TEXT
# and never print DONE.
pair() {
    local id="$1" what="$2"
    CTL_OK=0
    run "${id}c" "$3" "$5" "${8:-}"
    if [ "$CFG_OK" != 1 ]; then bad "${id}c" "fixture config is not JSON"; return; fi
    if [ "$RC" != 0 ] || ! has DONE; then
        skip "${id}c" "the control program does not run here (rc=$RC: $(head -c 160 <<<"$OUT" | tr '\n' ' '))"
        skip "$id" "no control"
        return
    fi
    ok "${id}c" "CONTROL: without the setting the program runs"; CTL_OK=1
    run "$id" "$4" "$5" "${8:-}"
    if [ "$CFG_OK" = 1 ] && [ "$RC" = "$6" ] && has "$7" && ! has DONE; then
        ok "$id" "$what"
    else
        bad "$id" "$what -- not refused as expected" "cfg=$CFG_OK rc=$RC (want $6) out=$(head -c 240 <<<"$OUT" | tr '\n' ' ')"
    fi
}

echo "== codegen caps"
CG='use codegen
main {
  let a = codegen.run("python", "x = 1\nx")
  let b = codegen.run("python", "y = 2222222222\ny")
  print("DONE")
}'
pair UL-01 "codegen.max_code_size_bytes refuses a longer code string" \
    ',"codegen":{"enabled":true}' ',"codegen":{"enabled":true,"max_code_size_bytes":10}' \
    "$CG" 1 "code exceeds maximum size"
pair UL-02 "codegen.max_cumulative_calls refuses the call past the cap" \
    ',"codegen":{"enabled":true}' ',"codegen":{"enabled":true,"max_cumulative_calls":1}' \
    "$CG" 1 "cumulative call limit reached"
pair UL-03 "codegen.max_cumulative_code_bytes refuses the call that crosses it" \
    ',"codegen":{"enabled":true}' ',"codegen":{"enabled":true,"max_cumulative_code_bytes":12}' \
    "$CG" 1 "cumulative code size limit reached"

echo ""
echo "== limits"
REC='fn r(n) { if n <= 0 { return 0 } return 1 + r(n - 1) }
main { print(r(40)) print("DONE") }'
pair UL-04 "limits.execution.call_depth is a HARD block on deeper recursion" \
    '' ',"limits":{"execution":{"call_depth":10}}' "$REC" 3 "exceeds limit of 10"
PY='main {
  let v = <<python
a = 1
b = 2
c = 3
d = 4
a + b + c + d
>>
  print(v) print("DONE")
}'
pair UL-05 "languages.per_language.python.max_lines is a HARD block on a longer block" \
    '' ',"languages":{"per_language":{"python":{"max_lines":3}}}' "$PY" 3 "python block has"
JS='main {
  let v = <<javascript
var a = 1;
var b = 2;
var c = 3;
a + b + c
>>
  print(v) print("DONE")
}'
# Python blocks run here when UL-05c passed; the "off" arms below use that to
# tell "this platform cannot run the block" from "the switch did not open it".
PY_OK=$CTL_OK
pair UL-06 "languages.per_language.javascript.max_lines is a HARD block on a longer block" \
    '' ',"languages":{"per_language":{"javascript":{"max_lines":2}}}' "$JS" 3 "javascript block has"

echo ""
echo "== restrictions on by default: the level decides whether the override passes"
OB='main {
  let v = <<python
import base64
p = base64.b64decode("MSsx")
f = getattr(__builtins__, "ev" + "al")
h = globals()["__builtins__"]
r = eval(p.decode())
r
>>
  print(v) print("DONE")
}'
VC='main {
  let v = <<python
cmd = "git log -p --all -S password"
len(cmd)
>>
  print(v) print("DONE")
}'
pair UL-07 "restrictions.obfuscation.level hard is not passed by --governance-override" \
    '' ',"restrictions":{"obfuscation":{"level":"hard"}}' "$OB" 3 "restrictions.obfuscation" --governance-override
pair UL-08 "restrictions.vcs_secret_extraction.level hard is not passed by --governance-override" \
    '' ',"restrictions":{"vcs_secret_extraction":{"level":"hard"}}' "$VC" 3 "restrictions.vcs_secret_extraction" --governance-override

# The two controls above pass through the override. These show the check
# itself is what refuses: at the default nothing else lets the block run.
offctl() { # ID KEY PROGRAM
    run "$1.def" '' "$3"
    local def_rc=$RC def_done=0; has DONE && def_done=1
    run "$1" ",\"restrictions\":{\"$2\":{\"enabled\":false}}" "$3"
    if { [ "$RC" != 0 ] || ! has DONE; } && [ "$PY_OK" = 1 ]; then
        bad "$1" "with restrictions.$2 enabled false the block is still refused" "rc=$RC out=$(head -c 200 <<<"$OUT" | tr '\n' ' ')"
    elif [ "$RC" != 0 ] || ! has DONE; then
        skip "$1" "Python blocks do not run here (rc=$RC)"
    elif [ "$def_rc" = 3 ] && [ "$def_done" = 0 ]; then
        ok "$1" "CONTROL: restrictions.$2 is what refuses -- enabled false runs, the default is exit 3"
    else
        bad "$1" "the default did not refuse the block" "default rc=$def_rc done=$def_done"
    fi
}
offctl UL-07d obfuscation "$OB"
offctl UL-08d vcs_secret_extraction "$VC"

echo ""
echo "== code_injection switches: each opens only its own pattern"
P_DYN='main {
  let v = <<python
r = eval("1+1")
r
>>
  print(v) print("DONE")
}'
P_CMD='main {
  let v = <<python
import subprocess
1
>>
  print(v) print("DONE")
}'
P_SQL='main {
  let v = <<python
name = "x"
q = "SELECT * FROM t WHERE n = " + chr(39) + "x" + chr(39)
s = "SELECT a FROM t WHERE b = \"" + name
len(q)
>>
  print(v) print("DONE")
}'
# switch ID KEY OWN_PROGRAM OTHER_PROGRAM
switch() {
    local on=",\"restrictions\":{\"code_injection\":{\"enabled\":true}}"
    local off=",\"restrictions\":{\"code_injection\":{\"enabled\":true,\"$2\":false}}"
    run "$1.on" "$on" "$3"
    local on_rc=$RC on_done=0; has DONE && on_done=1
    run "$1" "$off" "$3"
    if { [ "$RC" != 0 ] || ! has DONE; } && [ "$PY_OK" = 1 ]; then
        bad "$1" "with $2 false its own pattern is still refused" "rc=$RC out=$(head -c 200 <<<"$OUT" | tr '\n' ' ')"
        skip "${1}x" "no control"; return
    elif [ "$RC" != 0 ] || ! has DONE; then
        skip "$1" "Python blocks do not run here (rc=$RC)"
        skip "${1}x" "no control"; return
    fi
    if [ "$on_rc" = 3 ] && [ "$on_done" = 0 ]; then
        ok "$1" "restrictions.code_injection.$2: true refuses its pattern (exit 3), false runs it"
    else
        bad "$1" "$2 at its default did not refuse its own pattern" "rc=$on_rc done=$on_done"
    fi
    run "${1}x" "$off" "$4"
    if [ "$RC" = 3 ] && has "restrictions.code_injection" && ! has DONE; then
        ok "${1}x" "with $2 false, another switch's pattern is still refused"
    else
        bad "${1}x" "turning $2 off opened another switch's pattern" "rc=$RC out=$(head -c 200 <<<"$OUT" | tr '\n' ' ')"
    fi
}
switch UL-09a block_dynamic_code_gen "$P_DYN" "$P_CMD"
switch UL-09b block_command_injection "$P_CMD" "$P_DYN"
switch UL-09c block_sql_injection_patterns "$P_SQL" "$P_DYN"

echo ""
echo "== agent limits (stub)"
# agent DIR FIXTURE TOP_EXTRA AGENT_EXTRA PROGRAM -> OUT RC REQS CFG_OK D
agent() {
    D="$T/$1"; mkdir -p "$D"
    printf '%s\n' "$2" > "$D/fixture.json"
    REQS=0; OUT=""; RC=0; CFG_OK=0
    if ! start_stub "$D/fixture.json" "$D" >/dev/null 2>&1; then OUT="STUBFAIL"; return; fi
    local ag="\"provider\":\"gemini\",\"model\":\"stub\",\"api_base\":\"http://127.0.0.1:$STUB_PORT\",\"api_key_env\":\"FAKE_KEY_UL\",\"max_tokens\":100,\"max_turns\":30,\"system_prompt\":\"You review billing invoices for accuracy.\""
    printf '{%s,"circuit_breaker":{"enabled":false}%s,"agents":{"a":{%s%s},"b":{%s}}}\n' \
        "$BASE" "$3" "$ag" "$4" "$ag" > "$D/govern.json"
    python3 -c 'import json,sys; json.load(sys.stdin)' < "$D/govern.json" && \
    python3 -c 'import json,sys; json.load(sys.stdin)' < "$D/fixture.json" && CFG_OK=1
    printf '%s\n' "$5" > "$D/p.naab"
    OUT="$(cd "$D" && timeout 90s "$NAAB" run p.naab 2>&1)"; RC=$?
    stop_stub 2>/dev/null
    REQS=$(ls "$D"/req_*.json 2>/dev/null | wc -l | tr -d ' ')
}
TEXT_FX='{"responses":[{"content":"invoice ok","input_tokens":10,"output_tokens":20}]}'
SEND3='use agent
main {
  let h = agent.create("a")
  let r1 = agent.send(h, "check invoice one")
  let r2 = agent.send(h, "check invoice two")
  let r3 = agent.send(h, "check invoice three")
  print("DONE")
}'
agent ul10c "$TEXT_FX" ',"exposure_tracking":{"enabled":true,"level":"hard"}' '' "$SEND3"
if [ "$OUT" = STUBFAIL ] || [ "$CFG_OK" != 1 ] || [ "$RC" != 0 ] || [ "$REQS" != 3 ]; then
    skip "UL-10c" "three sends do not reach the stub here (rc=$RC reqs=$REQS: $(head -c 160 <<<"$OUT" | tr '\n' ' '))"
    skip "UL-10" "no control"; AGENT_OK=0
else
    ok "UL-10c" "CONTROL: with no action cap three sends make three provider calls"; AGENT_OK=1
    agent ul10 "$TEXT_FX" ',"exposure_tracking":{"enabled":true,"level":"hard","max_autonomous_actions":2}' '' "$SEND3"
    if [ "$CFG_OK" = 1 ] && [ "$RC" = 3 ] && [ "$REQS" = 2 ] && ! has DONE; then
        ok "UL-10" "exposure_tracking.max_autonomous_actions: the third send is refused and makes no call"
    else
        bad "UL-10" "the send past max_autonomous_actions was not refused" "rc=$RC reqs=$REQS out=$(head -c 200 <<<"$OUT" | tr '\n' ' ')"
    fi
fi

TWO='use agent
main {
  let h = agent.create("a")
  let r1 = agent.send(h, "check invoice one")
  let g = agent.create("b")
  let r2 = agent.send(g, "check invoice two")
  print("DONE")
}'
if [ "$AGENT_OK" = 1 ]; then
    agent ul11c "$TEXT_FX" ',"exposure_tracking":{"enabled":true,"level":"hard"}' '' "$TWO"
    if [ "$RC" != 0 ] || [ "$REQS" != 2 ]; then
        skip "UL-11c" "two agents do not both reach the stub (rc=$RC reqs=$REQS)"; skip "UL-11" "no control"
    else
        ok "UL-11c" "CONTROL: with no agent cap two agents each make a provider call"
        agent ul11 "$TEXT_FX" ',"exposure_tracking":{"enabled":true,"level":"hard","max_unique_agents":1}' '' "$TWO"
        # Refused at agent.create as a plain error (exit 1, catchable, no check
        # result recorded) -- pinned as it is, recorded as A168.
        if [ "$CFG_OK" = 1 ] && [ "$RC" = 1 ] && [ "$REQS" = 1 ] && has "max agent count reached" && ! has DONE; then
            ok "UL-11" "exposure_tracking.max_unique_agents: the second agent is refused at create and makes no call"
        else
            bad "UL-11" "a second agent past max_unique_agents was not refused" "rc=$RC reqs=$REQS out=$(head -c 200 <<<"$OUT" | tr '\n' ' ')"
        fi
    fi
else
    skip "UL-11c" "no stub"; skip "UL-11" "no stub"
fi

# The tool returns a marker, 120 filler characters, then a second marker. The
# observable is the request the stub received after the tool ran.
TOOLS=',"tools_enabled":true,"tools":["g"],"max_tool_loop_turns":10,"max_tool_calls_per_turn":10'
TOOL_PROG='use agent
fn g(q) { return "HEADMARK" + "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnop" + "TAILMARK" }
main {
  agent.register_tool("g", g, {"description":"d","parameters":{"q":{"type":"string","description":"q"}}})
  let h = agent.create("a")
  let r = agent.send(h, "check the invoices")
  print("DONE")
}'
ONE_FX='{"responses":[
 {"tool_calls":[{"name":"g","args":{"q":"1"}}],"input_tokens":10,"output_tokens":10},
 {"content":"invoices checked","input_tokens":10,"output_tokens":7}]}'
TWO_FX='{"responses":[
 {"tool_calls":[{"name":"g","args":{"q":"1"}}],"input_tokens":10,"output_tokens":10},
 {"tool_calls":[{"name":"g","args":{"q":"2"}}],"input_tokens":10,"output_tokens":10},
 {"content":"invoices checked","input_tokens":10,"output_tokens":7}]}'
got(){ grep -c -- "$1" "$D/req_$2.json" 2>/dev/null | head -1; }
if [ "$AGENT_OK" = 1 ]; then
    agent ul12c "$ONE_FX" '' "$TOOLS" "$TOOL_PROG"
    if [ "$RC" != 0 ] || [ "$REQS" != 2 ] || [ "$(got TAILMARK 2)" = 0 ]; then
        skip "UL-12c" "the tool's whole result does not reach the stub here (rc=$RC reqs=$REQS)"; skip "UL-12" "no control"
    else
        ok "UL-12c" "CONTROL: with no cap set the model receives the tool's whole result"
        agent ul12 "$ONE_FX" '' "$TOOLS,\"tool_result_max_chars\":20" "$TOOL_PROG"
        if [ "$CFG_OK" = 1 ] && [ "$REQS" = 2 ] && [ "$(got HEADMARK 2)" != 0 ] && [ "$(got TAILMARK 2)" = 0 ] && [ "$(got truncated 2)" != 0 ]; then
            ok "UL-12" "agents.<n>.tool_result_max_chars: the model receives the first 20 characters, marked truncated"
        else
            bad "UL-12" "the tool result past tool_result_max_chars reached the model" "rc=$RC reqs=$REQS head=$(got HEADMARK 2) tail=$(got TAILMARK 2) trunc=$(got truncated 2)"
        fi
    fi
    agent ul13c "$TWO_FX" '' "$TOOLS" "$TOOL_PROG"
    if [ "$RC" != 0 ] || [ "$REQS" != 3 ] || [ "$(grep -o TAILMARK "$D/req_3.json" | wc -l | tr -d ' ')" != 2 ]; then
        skip "UL-13c" "two whole tool results do not reach the stub here (rc=$RC reqs=$REQS)"; skip "UL-13" "no control"
    else
        ok "UL-13c" "CONTROL: with no total cap set the model receives both tool results whole"
        agent ul13 "$TWO_FX" '' "$TOOLS,\"tool_result_max_total_chars\":200" "$TOOL_PROG"
        n="$(grep -o TAILMARK "$D/req_3.json" 2>/dev/null | wc -l | tr -d ' ')"
        if [ "$CFG_OK" = 1 ] && [ "$REQS" = 3 ] && [ "$n" = 1 ] && [ "$(got 'cumulative tool result limit' 3)" != 0 ]; then
            ok "UL-13" "agents.<n>.tool_result_max_total_chars: the result crossing the total is withheld from the model"
        else
            bad "UL-13" "a tool result past tool_result_max_total_chars reached the model" "rc=$RC reqs=$REQS whole_results=$n"
        fi
    fi
else
    for a in UL-12c UL-12 UL-13c UL-13; do skip "$a" "no stub"; done
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
