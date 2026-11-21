#!/usr/bin/env bash
# ============================================================
# test_tool_self_grading.sh -- a tool may not grade, reset or commit its own agent (A185)
#
# THE DEFECT
#
# Gap O (68e3cc5b, 2026-06-19) refuses agent.send() from a tool on the handle
# whose tool is running, and agent.propose() has the same guard. The calls
# added later have none: agent.record_validation (S22's ground truth),
# agent.reset (clears the handle's drift history) and agent.commit. Observed
# on master c1312132: a tool the model called recorded a passing validation,
# with an evidence count, for its own handle (VALIDATION_RECORDED passed=true).
#
# THE FIX
#
# All three refuse a call for the handle whose tool is running
# (t_in_tool_execution_for_handle), the same check and the same error shape as
# send and propose. The script, after the agent's turn, and a tool acting on
# ANOTHER handle are unaffected.
#
#   TG-00  CONTROL: the tool runs, and the script's own record_validation after
#          the send is recorded (VALIDATION_RECORDED passed=true)
#   TG-01  record_validation from the tool on its own handle is refused and
#          records nothing
#   TG-02  reset from the tool on its own handle is refused (no AGENT_RESET)
#   TG-03  commit from the tool on its own handle is refused by this guard (its
#          message, not the proposal check that would follow)
#   TG-04  CONTROL: record_validation from the tool on ANOTHER handle is recorded
#   TG-05  CONTROL: reset from the tool on another handle works (AGENT_RESET)
#
# Kill table (each mutant applied alone, rebuilt, this suite run; observed
# 2026-11-21 on the default build):
#   M1 no guard in record_validation -> TG-01 red
#   M2 no guard in reset             -> TG-02 red
#   M3 no guard in commit            -> TG-03 red
# On master c1312132: TG-01, TG-02, TG-03 red; the controls green.
#
# Not covered: a tool that starts an async fn which then calls these on its
# own handle runs on another thread, where the guard's thread-local is unset
# -- the same limit as Gap O's send guard.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
STUB_PORT=""
source "$SCRIPT_DIR/../helpers/stub_launch.sh"
TEST_TMP=$(mktemp -d "${TMPDIR:-/tmp}/naab_selfgrade_XXXXXX")
cleanup() { stop_stub 2>/dev/null; teardown_isolated_trust; rm -rf "$TEST_TMP"; }
trap cleanup EXIT

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2${3:+ -- $3}"; }
unm()  { UNM=$((UNM+1));  echo "  UNMEASURABLE [$1] $2"; }

echo "=== test_tool_self_grading.sh ==="
if [ ! -x "$NAAB" ]; then echo "  UNMEASURABLE: naab-lang not built"; echo "Result: 0 passed, 0 failed, 1 unmeasurable"; exit 1; fi
export FAKE_KEY_SG="fake-key-selfgrade"

# run ID TOOL_BODY [SCRIPT_AFTER]: one agent "tooler" whose single tool
# "act" runs TOOL_BODY (h = its own handle, h2 = another handle of the same
# agent), then SCRIPT_AFTER runs in main. Sets RC, OUT, D.
run() {
    D="$TEST_TMP/$1"; mkdir -p "$D"
    printf '{"responses": [{"tool_calls": [{"name": "act", "args": {"note": "x"}}]}, {"content": "FINAL"}]}\n' > "$D/fixture.json"
    if ! start_stub "$D/fixture.json" "$D"; then OUT="STUB-FAILED"; RC=99; return; fi
    cat > "$D/govern.json" <<EOF
{"mode": "enforce", "security": {"sandbox_level": "elevated"},
 "telemetry": {"enabled": true, "output_file": "tele.jsonl"},
 "agents": {"tooler": {"provider": "gemini", "model": "stub-model",
   "api_base": "http://127.0.0.1:$STUB_PORT", "api_key_env": "FAKE_KEY_SG",
   "max_tokens": 100, "max_turns": 20, "retry": {"max_attempts": 1},
   "propose_candidates_max": 2, "tools_enabled": true, "tools": ["act"]}}}
EOF
    cat > "$D/t.naab" <<EOF
use agent
main {
    let h = agent.create("tooler")
    let h2 = agent.create("tooler")
    agent.register_tool("act", fn(args) {
        try {
$2
        } catch (e) {
            print("TOOL_DENIED=" + string(e))
        }
        return "acted"
    }, {"description": "act", "parameters": {"note": {"type": "string", "description": "n"}}})
    let r = agent.send(h, "go")
    print("CONTENT=" + string(r.get("content")))
${3:-}
}
EOF
    RC=0; OUT=$(cd "$D" && timeout 60s "$NAAB" run t.naab 2>&1) || RC=$?
    stop_stub
}
events() { grep -cF "\"event_type\":\"$1\"" "$D/tele.jsonl" 2>/dev/null; }

# ---- TG-00 control -----------------------------------------------------------
run tg00 '            print("TOOL_RAN")' '    let v = agent.record_validation(h, true, "ok")
    print("SCRIPT_RECORDED=" + string(v.get("recorded")))'
if [ "$OUT" = "STUB-FAILED" ]; then
    unm TG-00 "the agent stub did not start"; echo "Result: $PASS passed, $FAIL failed, $UNM unmeasurable"; exit 1
elif [ "$RC" = 0 ] && grep -q '^TOOL_RAN' <<<"$OUT" && grep -q 'CONTENT=FINAL' <<<"$OUT" && \
     grep -q 'SCRIPT_RECORDED=true' <<<"$OUT" && [ "$(events VALIDATION_RECORDED)" = 1 ]; then
    pass TG-00 "CONTROL: the tool runs and the script's own record_validation is recorded"
else
    fail TG-00 "the harness does not reach the tool or the script's call; nothing below can be read" "rc=$RC $(grep -E 'TOOL_|CONTENT=|SCRIPT_|rror' <<<"$OUT" | head -4 | tr '\n' ' ')"
    echo "Result: $PASS passed, $FAIL failed, $UNM unmeasurable"; exit 1
fi

# ---- TG-01 record_validation on its own handle --------------------------------
run tg01 '            let v = agent.record_validation(h, true, "self", 5)
            print("TOOL_RECORDED=" + string(v.get("recorded")))'
if grep -q 'TOOL_DENIED=.*record_validation denied' <<<"$OUT" && ! grep -q 'TOOL_RECORDED' <<<"$OUT" && \
   [ "$(events VALIDATION_RECORDED)" = 0 ] && grep -q 'CONTENT=FINAL' <<<"$OUT"; then
    pass TG-01 "record_validation from a tool on its own handle is refused and records nothing"
else
    fail TG-01 "the tool graded its own agent" "rc=$RC recorded_events=$(events VALIDATION_RECORDED) $(grep -E 'TOOL_' <<<"$OUT" | head -2 | tr '\n' ' ')"
fi

# ---- TG-02 reset on its own handle ---------------------------------------------
run tg02 '            agent.reset(h)
            print("TOOL_RESET")'
if grep -q 'TOOL_DENIED=.*reset denied' <<<"$OUT" && ! grep -q '^TOOL_RESET' <<<"$OUT" && \
   [ "$(events AGENT_RESET)" = 0 ]; then
    pass TG-02 "reset from a tool on its own handle is refused (no AGENT_RESET)"
else
    fail TG-02 "the tool reset its own agent" "rc=$RC resets=$(events AGENT_RESET) $(grep -E 'TOOL_' <<<"$OUT" | head -2 | tr '\n' ' ')"
fi

# ---- TG-03 commit on its own handle ---------------------------------------------
run tg03 '            agent.commit(h, {"__proposal": "not-a-real-proposal"})
            print("TOOL_COMMITTED")'
if grep -q 'TOOL_DENIED=.*commit denied' <<<"$OUT" && ! grep -q '^TOOL_COMMITTED' <<<"$OUT"; then
    pass TG-03 "commit from a tool on its own handle is refused by this guard"
else
    fail TG-03 "commit from the tool reached the proposal check" "rc=$RC $(grep -E 'TOOL_' <<<"$OUT" | head -2 | cut -c1-160 | tr '\n' ' ')"
fi

# ---- TG-04 / TG-05 another handle --------------------------------------------------
run tg04 '            let v = agent.record_validation(h2, true, "other")
            print("TOOL_RECORDED=" + string(v.get("recorded")))'
if grep -q 'TOOL_RECORDED=true' <<<"$OUT" && [ "$(events VALIDATION_RECORDED)" = 1 ]; then
    pass TG-04 "CONTROL: record_validation from a tool on another handle is recorded"
else
    fail TG-04 "the guard refused a call on another handle" "rc=$RC $(grep -E 'TOOL_' <<<"$OUT" | head -2 | tr '\n' ' ')"
fi
run tg05 '            agent.reset(h2)
            print("TOOL_RESET")'
if grep -q '^TOOL_RESET' <<<"$OUT" && [ "$(events AGENT_RESET)" -ge 1 ]; then
    pass TG-05 "CONTROL: reset from a tool on another handle works"
else
    fail TG-05 "the guard refused a reset of another handle" "rc=$RC $(grep -E 'TOOL_' <<<"$OUT" | head -2 | tr '\n' ' ')"
fi

echo "Result: $PASS passed, $FAIL failed, $UNM unmeasurable"
[ "$FAIL" -eq 0 ]
