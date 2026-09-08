#!/usr/bin/env bash
# A12 fix: bind an agent.create() handle's TOOL-driven file access to its
# govern.json role, closing the gap where per-agent blocked_paths / FS action
# matrix were enforced only against the CLI --agent-id.
#
# Mechanism: the four per-agent role gates (checkPathAccess / checkFilesystem
# Allowed / checkNetworkAllowed / checkShellAllowed) resolved role.name ==
# agent_id_, which the agent.create() runtime never sets. The fix routes them
# through effectiveAgentId(), which returns the acting tool role (set by
# ScopedToolContext around the tool callback) when a tool is executing, else
# agent_id_. So a created agent's tool is scoped by its role, while the
# surrounding orchestration script (which is NOT the agent) is not.
#
# LOAD-BEARING: AB-01 (tool blocked) is the fix; its negative control is the
# register evidence for A12 — before the fix the same tool read succeeded
# (TOOL_SUCCESS=true, PASS). AB-02 (no-leak) is the correctness guard that the
# role does not bind the orchestration script after the tool returns; without
# it the fix could pass by simply binding the whole thread. AB-03 proves the
# gate does not over-block a tool reading an allowed path.

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_agent_role_binding.sh"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
source "$SCRIPT_DIR/../helpers/stub_launch.sh"

PASS=0; FAIL=0
ok(){ echo "  PASS [$1] $2"; PASS=$((PASS+1)); }
bad(){ echo "  FAIL [$1] $2"; echo "        $3"; FAIL=$((FAIL+1)); }

setup_isolated_trust
TT=$(mktemp -d)
STUB_PID=""
cleanup(){ [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null; teardown_isolated_trust; rm -rf "$TT"; }
trap cleanup EXIT

"$NAAB" --keygen "$TT/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TT/k.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TT/k.pem"
export KA12ROLE=x
echo "SECRET_BLOCKED" > "$TT/blocked.txt"
echo "OK_ALLOWED"    > "$TT/allowed.txt"

# fixture: tool call read the path the harness substitutes
mk_run() {  # $1 = path for the tool to read, $2 = extra naab after send
  cat > "$TT/fixture.json" <<EOF
{"responses":[{"tool_calls":[{"name":"reader","args":{"p":"$1"}}]},{"content":"done","output_tokens":10}]}
EOF
  start_stub "$TT/fixture.json" "$TT" >/dev/null 2>&1 || return 1
  cat > "$TT/govern.json" <<EOF
{ "mode":"enforce","security":{"sandbox_level":"elevated"},
  "agents":{"restricted_agent":{"provider":"gemini","model":"stub-model","api_base":"http://127.0.0.1:$STUB_PORT","api_key_env":"KA12ROLE","max_turns":10,"tools_enabled":true,"tools":["reader"],"blocked_paths":["$TT/blocked.txt"]}} }
EOF
  (cd "$TT" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1)
  cat > "$TT/test.naab" <<EOF
use agent
use file
fn reader(p) { return file.read(p) }
main {
  agent.register_tool("reader", reader, {"description":"read","parameters":{"p":{"type":"string","description":"path"}}})
  let h = agent.create("restricted_agent")
  let r = agent.send(h, "go")
  let results = r.get("tool_results")
  if results != null { if results.length() > 0 { print("TOOL_SUCCESS=" + string(results[0].get("success"))) } }
  $2
}
EOF
  ( cd "$TT" && timeout 60 "$NAAB" test.naab 2>&1 ); local ec=$?
  stop_stub
  return $ec
}

# AB-01: tool reads the role-blocked path -> HARD block naming the agent
OUT=$(mk_run "$TT/blocked.txt" 'print("REACHED_END")'); EC=$?
if [ "$EC" = "3" ] && echo "$OUT" | grep -q "Agent 'restricted_agent' blocked from path"; then
    ok "AB-01" "agent tool reading a role-blocked path is HARD-blocked, naming the acting agent"
else
    bad "AB-01" "the tool read the role-blocked path (fix not active)" "exit $EC; $(echo "$OUT" | grep -iE 'TOOL_SUCCESS|SECRET|blocked' | head -2)"
fi

# AB-02: NO-LEAK — tool reads an ALLOWED path, then the SCRIPT reads the
# role-blocked path; the script is not the agent, so it must SUCCEED.
OUT=$(mk_run "$TT/allowed.txt" 'let c = file.read("'"$TT"'/blocked.txt") print("SCRIPT_READ=" + string(c))'); EC=$?
if echo "$OUT" | grep -q "SCRIPT_READ=SECRET_BLOCKED" && echo "$OUT" | grep -q "TOOL_SUCCESS=true"; then
    ok "AB-02" "NO-LEAK: the tool role does not bind the orchestration script after the tool returns"
else
    bad "AB-02" "the agent role leaked to the surrounding script (over-binding)" "exit $EC; $(echo "$OUT" | grep -iE 'SCRIPT_READ|blocked|Error' | head -2)"
fi

# AB-03: tool reads an ALLOWED path -> succeeds (no over-block)
OUT=$(mk_run "$TT/allowed.txt" 'print("END")'); EC=$?
if echo "$OUT" | grep -q "TOOL_SUCCESS=true"; then
    ok "AB-03" "CONTROL: a tool reading a non-blocked path still succeeds (no over-block)"
else
    bad "AB-03" "the fix over-blocks a permitted tool read" "exit $EC; $(echo "$OUT" | grep -iE 'TOOL_SUCCESS|blocked|Error' | head -2)"
fi

echo
echo "agent role binding: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
