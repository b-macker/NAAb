#!/usr/bin/env bash
# ============================================================
# test_run_budget_coverage.sh -- every provider call the agent module makes
# must answer to the run budgets (agent_dispatch.hard_stop.max_calls_per_run,
# max_tokens_per_run and max_agent_time_ms)   [A46]
#
# THE DEFECT
#
# The agent module makes provider calls at four sites: the send's own call,
# the tool loop's call helper (callToolLoopTurn, which also serves the final
# tool-less turn), the step-up challenge, and each agent.propose() candidate.
# F54 (#236) gave the first two one shared answer to "does this CALL fit the
# run budget" (chargeCallBudget()); the token half was never brought in, and
# the other two sites were never wired at all:
#
#   * tool loop: tokens of every call after the first never reached the run
#     total (agent.usage() 600/1407 while dispatch_status().tokens_used read
#     800, the first call only), so a tool-using agent could spend past
#     max_tokens_per_run without the stop ever setting;
#   * step-up challenge: incremented total_calls by hand and never compared it
#     with the cap, and its tokens reached telemetry only;
#   * propose: incremented total_calls by hand per candidate and never
#     compared it with the cap (max_calls_per_run: 1 let one propose make 3
#     calls), and added tokens without ever checking max_tokens_per_run.
#   * max_agent_time_ms: the run time total moved only for the send's own call,
#     so tool-loop turns, challenges and propose candidates spent time the cap
#     never saw (three ~400 ms tool-loop calls charged as 402 ms, three propose
#     candidates as 0).
#
# THE FIX
#   chargeTokenBudget() and chargeTimeBudget() sit beside chargeCallBudget();
#   every one of the four sites now charges each call through all three. chargeCallBudget() also refuses
#   a call once the run is already hard-stopped (a token or time stop set by an
#   earlier call), because the sites it guards do not all re-check the stop
#   between calls (the challenge is followed by the send's own call).
#
# OBSERVABLE: the number of requests the stub actually RECEIVED (req_N.json),
# not the engine's own counters -- a counter that is wrong is the defect.
#
#   RB-00  HARNESS CONTROL: one send under a generous budget reaches the stub
#          once and is charged exactly the fixture's tokens.
#   RB-01  tool loop crossing max_tokens_per_run stops before its next call
#          (2 requests, exit hard_stop).
#   RB-01c CONTROL: the same loop under a generous cap runs all 3 calls.
#   RB-02  tool-loop tokens are charged in full (tokens_used = all 3 calls).
#   RB-03  the final tool-less turn's tokens are charged too.
#   RB-04  propose under max_calls_per_run: 1 makes 1 call, not 3.
#   RB-04c CONTROL: propose under a generous cap makes all 3 candidates.
#   RB-05  propose crossing max_tokens_per_run stops generating.
#   RB-06  a step-up challenge past max_calls_per_run is refused (1 request).
#   RB-06c CONTROL: under a generous cap the challenge DOES fire (3 requests
#          for 2 sends) -- without it RB-06 passes whenever no challenge ran.
#   RB-07  a challenge's tokens are charged: crossing the cap refuses the
#          send's own call that follows it.
#   RB-08  after a refusal, calls_made equals the requests the provider got
#          (a refused call was counted: calls_made read one past reality).
#   RB-09c CONTROL: a tool loop of replies held HOLD ms each runs all 3 calls
#          under a generous max_agent_time_ms.
#   RB-09b every tool-loop call's TIME reaches the run total (>= 3 x HOLD).
#   RB-09  the held loop crossing max_agent_time_ms stops before its 3rd call.
#   RB-10  propose crossing max_agent_time_ms keeps the 2 candidates it paid
#          for and makes no 3rd call.
#   RB-11  a challenge's time is charged: crossing the cap refuses the send's
#          own call that follows it.
#   RB-TW  --tree-walk is refused under enforce (Bundle 2) and makes no
#          provider call (it used to prove the tree-walker reaches the budget).
# ============================================================
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../helpers/tree_walk_refusal.sh"
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
( cd "$T" && "$NAAB" --keygen k.pem >/dev/null 2>&1 && "$NAAB" --trust-key k.pem.pub >/dev/null 2>&1 )
export NAAB_SIGNING_KEY="$T/k.pem"; export FAKE_KEY_RB=fake

TOOLS=',"tools_enabled":true,"tools":["g"],"max_tool_loop_turns":10,"max_tool_calls_per_turn":10'
NO_CB='{"enabled":false}'
STEPUP_CB='{"enabled":true,"step_up_enabled":true,"step_up_min_words":3,"step_up_keyword_threshold":0.01}'

# arm DIR FIXTURE HARD_STOP_JSON AGENT_EXTRA CB_JSON PROGRAM [ENGINE]
# -> sets OUT, RC, REQS (requests the stub received), CFG_OK
arm() {
    local d="$T/$1"; mkdir -p "$d"
    printf '%s\n' "$2" > "$d/fixture.json"
    REQS=0; OUT=""; RC=0; CFG_OK=0
    if ! start_stub "$d/fixture.json" "$d" >/dev/null 2>&1; then OUT="STUBFAIL"; return; fi
    cat > "$d/govern.json" <<EOF
{"mode":"enforce","security":{"sandbox_level":"elevated"},
 "agent_dispatch":{"hard_stop":$3},
 "circuit_breaker":$5,
 "agents":{"a":{"provider":"gemini","model":"stub","api_base":"http://127.0.0.1:$STUB_PORT",
   "api_key_env":"FAKE_KEY_RB","max_tokens":100,"max_turns":30,
   "system_prompt":"You review billing invoices for accuracy."$4}}}
EOF
    # Validate the generated fixture before trusting any verdict from it: an
    # invalid config exits 4 and every refusal arm would pass for free.
    python3 -c 'import json,sys; json.load(sys.stdin)' < "$d/govern.json" && \
    python3 -c 'import json,sys; json.load(sys.stdin)' < "$d/fixture.json" && CFG_OK=1
    (cd "$d" && "$NAAB" --sign-governance >/dev/null 2>&1) || true
    printf '%s\n' "$6" > "$d/t.naab"
    local a=("$NAAB"); [ "${7:-vm}" = tw ] && a+=(--tree-walk); a+=(t.naab)
    OUT="$(cd "$d" && timeout 90s "${a[@]}" 2>&1)"; RC=$?
    stop_stub 2>/dev/null
    REQS=$(ls "$d"/req_*.json 2>/dev/null | wc -l | tr -d ' ')
}
val() { grep -oE "$1=[^ ]*" <<<"$OUT" | head -1 | cut -d= -f2-; }

DISPATCH='  let d = agent.dispatch_status()
  print("CALLS=" + string(d.get("calls_made")) + " TOKENS=" + string(d.get("tokens_used")) + " STOPPED=" + string(d.get("hard_stopped")))'

LOOP_PROG="use agent
fn g(q) { return \"ok\" }
main {
  agent.register_tool(\"g\", g, {\"description\":\"d\",\"parameters\":{\"q\":{\"type\":\"string\",\"description\":\"q\"}}})
  let h = agent.create(\"a\")
  let r = agent.send(h, \"check the invoices\")
  print(\"EXIT=\" + string(r.get(\"tool_loop_exit_reason\")) + \" FINAL=\" + string(r.get(\"tool_loop_final_turn\")))
$DISPATCH
}"
LOOP_FX='{"responses":[
 {"tool_calls":[{"name":"g","args":{"q":"1"}}],"input_tokens":100,"output_tokens":700},
 {"tool_calls":[{"name":"g","args":{"q":"2"}}],"input_tokens":200,"output_tokens":700},
 {"content":"invoices checked","input_tokens":300,"output_tokens":7}]}'

echo "== harness control"
arm rb00 '{"responses":[{"content":"invoice ok","input_tokens":10,"output_tokens":20}]}' \
    '{"max_calls_per_run":10,"max_tokens_per_run":100000}' "" "$NO_CB" "use agent
main {
  let h = agent.create(\"a\")
  let r = agent.send(h, \"check\")
$DISPATCH
}"
if [ "$CFG_OK" = 1 ] && [ "$RC" = 0 ] && [ "$REQS" = 1 ] && [ "$(val TOKENS)" = 30 ] && [ "$(val CALLS)" = 1 ]; then
    ok "RB-00" "one send reaches the stub once and is charged exactly its 30 tokens"
else
    bad "RB-00" "harness does not measure what it claims" "cfg=$CFG_OK rc=$RC reqs=$REQS out=${OUT:0:200}"
fi

echo ""
echo "== tool loop (callToolLoopTurn: the loop and the final tool-less turn)"
arm rb01 "$LOOP_FX" '{"max_tokens_per_run":1000}' "$TOOLS" "$NO_CB" "$LOOP_PROG"
if [ "$CFG_OK" = 1 ] && [ "$REQS" = 2 ] && [ "$(val EXIT)" = hard_stop ] && [ "$(val STOPPED)" = true ]; then
    ok "RB-01" "the tool loop stops at max_tokens_per_run: 2 requests, exit hard_stop"
else
    bad "RB-01" "the tool loop spent past max_tokens_per_run" "reqs=$REQS exit=$(val EXIT) stopped=$(val STOPPED) tokens=$(val TOKENS) rc=$RC"
fi

arm rb01c "$LOOP_FX" '{"max_tokens_per_run":100000}' "$TOOLS" "$NO_CB" "$LOOP_PROG"
if [ "$CFG_OK" = 1 ] && [ "$REQS" = 3 ] && [ "$(val EXIT)" = text_response ] && [ "$(val STOPPED)" = false ]; then
    ok "RB-01c" "CONTROL: under a generous cap the same loop runs all 3 calls"
else
    bad "RB-01c" "a generous cap refused tool-loop calls" "reqs=$REQS exit=$(val EXIT) stopped=$(val STOPPED) rc=$RC"
fi
# 100+700 + 200+700 + 300+7
if [ "$(val TOKENS)" = 2007 ]; then
    ok "RB-02" "every tool-loop call's tokens reach the run total (2007)"
else
    bad "RB-02" "tool-loop tokens missing from the run total" "tokens_used=$(val TOKENS), expected 2007"
fi

arm rb03 '{"responses":[
 {"tool_calls":[{"name":"g","args":{"q":"1"}}],"input_tokens":10,"output_tokens":10},
 {"tool_calls":[{"name":"g","args":{"q":"2"}}],"input_tokens":20,"output_tokens":20},
 {"content":"final answer","input_tokens":30,"output_tokens":30}]}' \
    '{"max_tokens_per_run":100000}' ',"tools_enabled":true,"tools":["g"],"max_tool_loop_turns":1,"max_tool_calls_per_turn":10' \
    "$NO_CB" "$LOOP_PROG"
if [ "$CFG_OK" = 1 ] && [ "$REQS" = 3 ] && [ "$(val FINAL)" = granted ] && [ "$(val TOKENS)" = 120 ]; then
    ok "RB-03" "the final tool-less turn ran and its tokens were charged (120)"
elif [ "$REQS" = 3 ] && [ "$(val FINAL)" = granted ]; then
    bad "RB-03" "the final tool-less turn's tokens missing from the run total" "tokens_used=$(val TOKENS), expected 120"
else
    skip "RB-03" "the final tool-less turn did not run here (reqs=$REQS final=$(val FINAL)); arm measures nothing"
fi

echo ""
echo "== propose"
PROP_PROG="use agent
main {
  let h = agent.create(\"a\")
  let p = agent.propose(h, \"summarize the invoices\", 3)
  let c = p.get(\"candidates\") ?? []
  let n = 0
  let i = 0
  while i < c.length() {
    if c[i].get(\"success\") ?? false { n = n + 1 }
    i = i + 1
  }
  print(\"SUCC=\" + string(n))
$DISPATCH
}"
PROP_FX='{"responses":[{"content":"Invoice totals reconciled for billing review.","input_tokens":20,"output_tokens":20}]}'
arm rb04 "$PROP_FX" '{"max_calls_per_run":1}' ',"propose_candidates_max":3' "$NO_CB" "$PROP_PROG"
if [ "$CFG_OK" = 1 ] && [ "$REQS" = 1 ] && [ "$(val STOPPED)" = true ]; then
    ok "RB-04" "propose under max_calls_per_run 1 makes 1 call and stops the run"
else
    bad "RB-04" "propose made calls past max_calls_per_run" "reqs=$REQS stopped=$(val STOPPED) calls=$(val CALLS) succ=$(val SUCC) rc=$RC"
fi
R4="$(val CALLS)/$REQS"
arm rb04c "$PROP_FX" '{"max_calls_per_run":10}' ',"propose_candidates_max":3' "$NO_CB" "$PROP_PROG"
if [ "$CFG_OK" = 1 ] && [ "$REQS" = 3 ] && [ "$(val SUCC)" = 3 ] && [ "$(val STOPPED)" = false ]; then
    ok "RB-04c" "CONTROL: under a generous cap propose makes all 3 candidates"
else
    bad "RB-04c" "a generous cap refused propose candidates" "reqs=$REQS succ=$(val SUCC) stopped=$(val STOPPED) rc=$RC out=${OUT:0:160}"
fi
# 40 tokens each: 40 <= 50, 80 > 50 sets the stop, the third is refused
arm rb05 "$PROP_FX" '{"max_tokens_per_run":50}' ',"propose_candidates_max":3' "$NO_CB" "$PROP_PROG"
if [ "$CFG_OK" = 1 ] && [ "$REQS" = 2 ] && [ "$(val STOPPED)" = true ]; then
    ok "RB-05" "propose stops generating once max_tokens_per_run is crossed (2 requests)"
else
    bad "RB-05" "propose spent past max_tokens_per_run" "reqs=$REQS stopped=$(val STOPPED) tokens=$(val TOKENS) rc=$RC"
fi

echo ""
echo "== step-up challenge (expired standing lease forces one on the 2nd send)"
CH_PROG="use agent
main {
  let h = agent.create(\"a\")
  let r1 = agent.send(h, \"check the invoices\")
  try {
    let r2 = agent.send(h, \"check them again\")
    print(\"SEND2=ok\")
  } catch (e) {
    print(\"SEND2=refused\")
  }
$DISPATCH
}"
CH_FX='{"responses":[
 {"content":"I review billing invoices for accuracy every day.","input_tokens":200,"output_tokens":200},
 {"content":"I review billing invoices for accuracy and flag errors.","input_tokens":400,"output_tokens":400},
 {"content":"I review billing invoices for accuracy again today.","input_tokens":200,"output_tokens":200}]}'
arm rb06c "$CH_FX" '{"max_calls_per_run":10}' ',"standing_lease_turns":1' "$STEPUP_CB" "$CH_PROG"
CH_FIRES=0
if [ "$CFG_OK" = 1 ] && [ "$REQS" = 3 ] && [ "$(val SEND2)" = ok ]; then
    CH_FIRES=1
    ok "RB-06c" "CONTROL: the challenge fires (3 requests for 2 sends) and the send proceeds"
else
    bad "RB-06c" "the challenge did not fire as staged; RB-06/RB-07 would measure nothing" "reqs=$REQS send2=$(val SEND2) rc=$RC out=${OUT:0:200}"
fi
if [ "$CH_FIRES" = 1 ]; then
    arm rb06 "$CH_FX" '{"max_calls_per_run":1}' ',"standing_lease_turns":1' "$STEPUP_CB" "$CH_PROG"
    if [ "$CFG_OK" = 1 ] && [ "$REQS" = 1 ] && [ "$(val SEND2)" = refused ]; then
        ok "RB-06" "a challenge past max_calls_per_run is refused (1 request)"
    else
        bad "RB-06" "the challenge call went past max_calls_per_run" "reqs=$REQS send2=$(val SEND2) calls=$(val CALLS) rc=$RC"
    fi
    R6="$(val CALLS)/$REQS"
    # 400 (send 1) + 800 (challenge) = 1200 > 1000: the send's own call must be refused
    arm rb07 "$CH_FX" '{"max_tokens_per_run":1000}' ',"standing_lease_turns":1' "$STEPUP_CB" "$CH_PROG"
    if [ "$CFG_OK" = 1 ] && [ "$REQS" = 2 ] && [ "$(val SEND2)" = refused ] && [ "$(val TOKENS)" = 1200 ]; then
        ok "RB-07" "a challenge's tokens are charged; crossing the cap refuses the send that follows"
    else
        bad "RB-07" "challenge tokens not charged, or the send after the stop still ran" "reqs=$REQS send2=$(val SEND2) tokens=$(val TOKENS) rc=$RC"
    fi
else
    skip "RB-06" "challenge not staged (see RB-06c)"; skip "RB-07" "challenge not staged (see RB-06c)"
fi

echo ""
echo "== calls_made counts calls made, not calls refused"
# RB-04 (propose) and RB-06 (challenge) each end in a refusal. calls_made must
# equal what the provider received: a refused call is not made.
if [ "${R4:-x}" = "1/1" ] && [ "${R6:-x}" = "1/1" ]; then
    ok "RB-08" "after a refusal, calls_made equals the requests the provider received"
elif [ "${CH_FIRES:-0}" != 1 ]; then
    skip "RB-08" "challenge not staged (see RB-06c)"
else
    bad "RB-08" "calls_made counts refused calls" "propose calls/reqs=${R4:-?} challenge calls/reqs=${R6:-?} (want 1/1)"
fi

echo ""
echo "== max_agent_time_ms (the third run budget; same four sites)"
# Each reply is held HOLD ms by the stub. The cap sits between one hold and
# two: overhead only ADDS time, so two calls are always past it (no flake in
# that direction); the first call stays under it unless its overhead exceeds
# CAP-HOLD ms.
HOLD=600; TCAP=1000
TLOOP_FX='{"responses":[
 {"tool_calls":[{"name":"g","args":{"q":"1"}}],"input_tokens":1,"output_tokens":1,"hold_ms":'$HOLD'},
 {"tool_calls":[{"name":"g","args":{"q":"2"}}],"input_tokens":1,"output_tokens":1,"hold_ms":'$HOLD'},
 {"content":"invoices checked","input_tokens":1,"output_tokens":1,"hold_ms":'$HOLD'}]}'
TDISPATCH='  let d = agent.dispatch_status()
  print("CALLS=" + string(d.get("calls_made")) + " TIME=" + string(d.get("agent_time_ms")) + " STOPPED=" + string(d.get("hard_stopped")) + " WHY=" + string(d.get("stop_reason")))'
TLOOP_PROG="${LOOP_PROG/"$DISPATCH"/"$TDISPATCH"}"
arm rb09c "$TLOOP_FX" '{"max_agent_time_ms":100000}' "$TOOLS" "$NO_CB" "$TLOOP_PROG"
T9C=0
if [ "$CFG_OK" = 1 ] && [ "$REQS" = 3 ] && [ "$(val EXIT)" = text_response ] && [ "$(val STOPPED)" = false ]; then
    T9C=1
    ok "RB-09c" "CONTROL: under a generous time cap the held loop runs all 3 calls"
else
    bad "RB-09c" "a generous time cap refused tool-loop calls (or the hold fixture did not run)" "reqs=$REQS exit=$(val EXIT) stopped=$(val STOPPED) rc=$RC"
fi
if [ "$T9C" = 1 ] && [ "$(val TIME)" -ge $((3 * HOLD)) ] 2>/dev/null; then
    ok "RB-09b" "every tool-loop call's time reaches the run total ($(val TIME) ms >= $((3 * HOLD)))"
else
    bad "RB-09b" "tool-loop time missing from the run total" "agent_time_ms=$(val TIME), expected >= $((3 * HOLD))"
fi
arm rb09 "$TLOOP_FX" "{\"max_agent_time_ms\":$TCAP}" "$TOOLS" "$NO_CB" "$TLOOP_PROG"
if [ "$CFG_OK" = 1 ] && [ "$REQS" = 2 ] && [ "$(val EXIT)" = hard_stop ] && grep -q max_agent_time_ms <<<"$(val WHY)"; then
    ok "RB-09" "the tool loop stops at max_agent_time_ms: 2 requests, exit hard_stop"
elif [ "$REQS" = 1 ]; then
    skip "RB-09" "the FIRST call alone exceeded ${TCAP} ms here (overhead > $((TCAP - HOLD)) ms); arm measures nothing"
else
    bad "RB-09" "the tool loop spent past max_agent_time_ms" "reqs=$REQS exit=$(val EXIT) time=$(val TIME) why=$(val WHY) rc=$RC"
fi

TPROP_FX='{"responses":[{"content":"Invoice totals reconciled for billing review.","input_tokens":1,"output_tokens":1,"hold_ms":'$HOLD'}]}'
TPROP_PROG="${PROP_PROG/"$DISPATCH"/"$TDISPATCH"}"
arm rb10 "$TPROP_FX" "{\"max_agent_time_ms\":$TCAP}" ',"propose_candidates_max":3' "$NO_CB" "$TPROP_PROG"
if [ "$CFG_OK" = 1 ] && [ "$REQS" = 2 ] && [ "$(val SUCC)" = 2 ] && grep -q max_agent_time_ms <<<"$(val WHY)"; then
    ok "RB-10" "propose stops generating once max_agent_time_ms is crossed (2 requests, 2 candidates kept)"
elif [ "$REQS" = 1 ]; then
    skip "RB-10" "the FIRST call alone exceeded ${TCAP} ms here; arm measures nothing"
else
    bad "RB-10" "propose spent past max_agent_time_ms" "reqs=$REQS succ=$(val SUCC) time=$(val TIME) why=$(val WHY) rc=$RC"
fi

if [ "$CH_FIRES" = 1 ]; then
    TCH_FX="$(sed 's/"output_tokens":\([0-9]*\)}/"output_tokens":\1,"hold_ms":'$HOLD'}/g' <<<"$CH_FX")"
    TCH_PROG="${CH_PROG/"$DISPATCH"/"$TDISPATCH"}"
    # send 1 (~HOLD) + challenge (~HOLD) > TCAP: the send's own call must be refused
    arm rb11 "$TCH_FX" "{\"max_agent_time_ms\":$TCAP}" ',"standing_lease_turns":1' "$STEPUP_CB" "$TCH_PROG"
    if [ "$CFG_OK" = 1 ] && [ "$REQS" = 2 ] && [ "$(val SEND2)" = refused ] && grep -q max_agent_time_ms <<<"$(val WHY)"; then
        ok "RB-11" "a challenge's time is charged; crossing the cap refuses the send that follows"
    elif [ "$REQS" = 1 ]; then
        skip "RB-11" "the FIRST call alone exceeded ${TCAP} ms here; arm measures nothing"
    else
        bad "RB-11" "challenge time not charged, or the send after the stop still ran" "reqs=$REQS send2=$(val SEND2) time=$(val TIME) why=$(val WHY) rc=$RC"
    fi
else
    skip "RB-11" "challenge not staged (see RB-06c)"
fi

echo ""
echo "== the tree-walker makes no call under enforce"
# RB-TW ran RB-01 on the tree-walker. Under enforce it is now refused before
# anything runs (Bundle 2), so it reaches no budget code and no provider.
arm rbtw "$LOOP_FX" '{"max_tokens_per_run":1000}' "$TOOLS" "$NO_CB" "$LOOP_PROG" tw
if [ "$CFG_OK" = 1 ] && tw_refused "$RC" "$OUT" && [ "$REQS" = 0 ]; then
    ok "RB-TW" "--tree-walk is refused under enforce; the stub received no request"
else
    bad "RB-TW" "--tree-walk should be refused under enforce" "reqs=$REQS rc=$RC"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP unmeasurable"
[ "$FAIL" -eq 0 ]
