#!/usr/bin/env bash
# test_tool_loop_reply.sh -- what a tool loop hands back (A162, F52).
#
# A162: after a tool loop, agent.send()'s trace.model was empty (and
#   api_key_env): the trace read fields only the send's FIRST call set, and the
#   reply returned is the loop turn's (observed with the stub, trace.model=[]).
#   The loop's call now fills them as the send's own call does.
# F52: a tool argument that is an object or an array reached the NAAb function
#   as JSON TEXT (observed: type string, '{"a":1,"b":[2,3]}'); it is now a dict
#   or a list, as json.parse() makes it. The comment "Parse arguments with depth
#   guard (Gap J)" came with 2d3a9d64 and no check did; the arguments are now
#   read within the shared nesting bound (bounded_jsonl.h, 64 levels) first, and
#   a call nested deeper is refused with an error result (TR-04).
#
#   TR-00  CONTROL: the tool ran and the reply arrived.
#   TR-01  trace.model and trace.api_key_env name the loop turn's call.
#   TR-01c CONTROL: a plain send's trace (it always had them).
#   TR-02  an object argument is a dict; TR-03 an array argument is a list.
#   TR-02c CONTROL: a string argument is still a string.
#   TR-04  arguments nested past 64 levels are refused (error result); the
#          send still completes.
#
# Kill table (mutants of src/stdlib/agent_impl.cpp, run by hand):
#   T1 the loop's call sets no actual_model             -> TR-01
#   T2 the loop's call sets no actual_api_key_env       -> TR-01
#   T3 nested arguments passed as JSON text again       -> TR-02, TR-03
#   T4 no nesting bound before parsing                  -> TR-04

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
W=$(mktemp -d "${TMPDIR:-/tmp}/naab_tlr_XXXXXX")
PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | tail -10 | sed 's/^/       -> /'; }
unm()  { UNM=$((UNM+1));   echo "  UNMEASURABLE [$1] $2"; }

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_tool_loop_reply.sh"
source "$SCRIPT_DIR/../helpers/stub_launch.sh"
source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust
trap 'stop_stub; teardown_isolated_trust; rm -rf "$W"' EXIT
[ -x "$NAAB" ] || { echo "  naab-lang not built: UNMEASURABLE, not a pass"; exit 1; }
export TLR_KEY="x"

mkfix() { python3 -c 'import json,sys; json.dump({"responses":[json.loads(a) for a in sys.argv[2:]]}, open(sys.argv[1],"w",encoding="utf-8"))' "$@"; }
N=0
# run_case TOOLS(on|off) MAIN_BODY FIXTURE_SPECS... -> RC, OUT
run_case() {
    local tools="$1" body="$2"; shift 2
    N=$((N+1)); local D="$W/c$N"; mkdir -p "$D"
    mkfix "$D/fixture.json" "$@"
    if ! start_stub "$D/fixture.json" "$D"; then stop_stub; RC=-1; OUT="stub did not start"; return; fi
    local t=''
    [ "$tools" = on ] && t=', "tools_enabled": true, "tools": ["note"], "max_turns": 10'
    cat > "$D/govern.json" <<EOF
{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" },
  "agents": { "worker": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT", "api_key_env": "TLR_KEY", "max_tokens": 50$t } } }
EOF
    printf 'use agent\nfn note(s) {\n    print("TOOL_GOT type=" + type(s))\n    if type(s) == "dict" { print("FIELD_A=" + string(s["a"])) }\n    if type(s) == "array" { print("LEN=" + string(len(s))) }\n    return "noted"\n}\nmain {\n    agent.register_tool("note", note, {"description": "note", "parameters": {"s": {"type": "object", "description": "x"}}})\n%s\n}\n' "$body" > "$D/p.naab"
    OUT=$(cd "$D" && timeout 120 "$NAAB" p.naab </dev/null 2>&1); RC=$?
    stop_stub
}
SEND='    let h = agent.create("worker")
    let r = agent.send(h, "go")
    print("REPLY=" + r["content"])
    print("MODEL=[" + r["trace"]["model"] + "] KEY=[" + r["trace"]["api_key_env"] + "]")'

echo "=== what a tool loop hands back ==="
run_case on "$SEND" '{"tool_calls": [{"name": "note", "args": {"s": "hi"}}]}' '{"content": "done"}'
if [ "$RC" -eq -1 ]; then unm TR-00 "the agent stub did not start"
elif [ "$RC" -eq 0 ] && grep -q '^TOOL_GOT type=string$' <<<"$OUT" && grep -q '^REPLY=done$' <<<"$OUT"; then
    pass TR-00 "CONTROL: the tool ran and the reply arrived"
    pass TR-02c "CONTROL: a string argument is still a string"
    if grep -qF 'MODEL=[stub-model] KEY=[TLR_KEY]' <<<"$OUT"; then pass TR-01 "after a tool loop the trace names the loop turn's model and key"
    else fail TR-01 "the trace after a tool loop is empty" "$OUT"; fi
else fail TR-00 "the tool loop did not run (rc=$RC) -- TR-01/TR-02c UNMEASURABLE" "$OUT"; fi

run_case off "$SEND" '{"content": "plain"}'
if [ "$RC" -eq 0 ] && grep -qF 'MODEL=[stub-model] KEY=[TLR_KEY]' <<<"$OUT"; then pass TR-01c "CONTROL: a plain send's trace names the model and key"
else fail TR-01c "the plain send's trace (rc=$RC)" "$OUT"; fi

run_case on "$SEND" '{"tool_calls": [{"name": "note", "args": {"s": {"a": 7, "b": [2, 3]}}}]}' '{"content": "done"}'
if [ "$RC" -eq 0 ] && grep -q '^TOOL_GOT type=dict$' <<<"$OUT" && grep -q '^FIELD_A=7$' <<<"$OUT"; then
    pass TR-02 "an object argument reaches the tool as a dict"
else fail TR-02 "the object argument (rc=$RC)" "$OUT"; fi

run_case on "$SEND" '{"tool_calls": [{"name": "note", "args": {"s": [1, 2, 3]}}]}' '{"content": "done"}'
if [ "$RC" -eq 0 ] && grep -q "^TOOL_GOT type=array$" <<<"$OUT" && grep -q '^LEN=3$' <<<"$OUT"; then
    pass TR-03 "an array argument reaches the tool as an array (a NAAb list)"
else fail TR-03 "the array argument (rc=$RC)" "$OUT"; fi

deep=$(python3 -c 'import json; v="x"
for _ in range(100): v={"a": v}
print(json.dumps({"tool_calls": [{"name": "note", "args": {"s": v}}]}))')
run_case on "$SEND" "$deep" '{"content": "done"}'
if [ "$RC" -eq 0 ] && ! grep -q '^TOOL_GOT' <<<"$OUT" && grep -q '^REPLY=done$' <<<"$OUT"; then
    pass TR-04 "arguments nested past the bound are refused; the send completes"
else fail TR-04 "the deeply nested call ran or the send failed (rc=$RC)" "$OUT"; fi

echo
echo "  Passed: $PASS  Failed: $FAIL  Unmeasurable: $UNM"
[ "$FAIL" -eq 0 ] && [ "$UNM" -eq 0 ]
