#!/usr/bin/env bash
# ============================================================
# test_tool_call_loop.sh -- B7 (Bundle 19 PR 5, the owner's decision 30 of
# 2026-11-27): S1 (circular_actions) also fires when the agent makes the same
# tool calls and gets the same results turn after turn, whatever its reply says.
#
# S1's fingerprint holds each reply's content hash, so an agent that repeated a
# tool call identically while rewording its reply never repeated: on 649caf69,
# 10 identical calls with identical results drew no S1 firing (observed). The
# record proposed marking the agent's own events; that alone could not catch
# it, because the reply hash still differs. Now each turn also gets a signature
# of what the agent did -- every tool call by name and arguments, the result it
# got, and what the tool itself did -- built from the events marked as raised
# inside the agent's turn (AgentTurnEventScope), and S1 fires when the last
# circular_lookback (3) turns' signatures are the same and non-empty.
#
# The mark is load-bearing under event_feed "since_last_check" only: there a
# script's events between sends reach the next check, carrying the agent's
# handle, and a script that varied its own actions would hide the loop (TC-05).
# Under the default feed they land in a bucket already analysed (traced,
# governance_engine.cpp's event feed), so TC-04 and TC-05 on the default feed
# would pass without it.
#
#   TC-00  CONTROL: identical replies fire S1 -- S1 is live and the telemetry
#          is read correctly
#   TC-00b CDD_TURN's circular_kind names that firing "reply"
#   TC-01  the same call with the same result each turn fires S1, kind
#          "tool_calls", first on the third such turn, every turn after
#   TC-02  CONTROL: the same call whose result changes each turn (a re-read
#          of something that changed) does not fire
#   TC-03  CONTROL: a call with different arguments each turn does not fire,
#          though the tool answers every call the same way (so the arguments,
#          not the result, are what tell the calls apart)
#   TC-04  the loop's tool reads the environment itself: still fires (what the
#          tool did is part of the turn, and it did the same)
#   TC-05  event_feed since_last_check, the script reads a different variable
#          between sends: still fires -- the script's events are not the agent's
#   TC-05c CONTROL: the same feed without the script's reads fires
#   TC-06  CONTROL: S1 switched off for the agent (context_drift_signals) draws
#          no firing on the loop
#
# KILL TABLE (each mutant run 2026-11-27, one build each; each turned its arms red)
#   T1  toolCallsRepeat() always false                       -> TC-01, TC-04, TC-05, TC-05c
#   T2  the TOOL_RESULT fingerprint is empty (result unread)  -> TC-02
#   T3  the TOOL_CALL fingerprint is the name alone           -> TC-03
#   T4  the in_agent_turn check is dropped                    -> TC-05
#   T5  agentSend() opens no AgentTurnEventScope             -> TC-01, TC-04, TC-05, TC-05c
#   T6  two of the last three turns are enough (the reply's  -> TC-01, TC-04, TC-05, TC-05c
#       rule)
# ============================================================
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"; isolate_trust_store
source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_tool_call_loop.sh"
source "$SCRIPT_DIR/../helpers/stub_launch.sh"

PASS=0; FAIL=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> ${3:0:400}"; }
[ -x "$NAAB" ] || { echo "naab-lang not built"; exit 1; }
T="$(mktemp -d "${TMPDIR:-/tmp}/naab_toolloop_XXXXXX")"
cleanup() { stop_stub 2>/dev/null; [ -n "${TL_KEEP:-}" ] || rm -rf "$T"; }
trap cleanup EXIT
export FAKE_KEY_TL="fake-key-tool-loop"

# fixture SHAPE FILE: 12 replies; from the third send on, each send first asks
# for one `lookup` call. loop shapes call it with the same argument each time.
fixture() {
python3 - "$1" "$2" <<'PYEOF'
import json, sys
shape, path = sys.argv[1], sys.argv[2]
r = []
for i in range(12):
    if i >= 2 and shape != "same_reply":
        args = {"k": "item%d" % i} if shape == "comply" else {"k": "same"}
        r.append({"tool_calls": [{"name": "lookup", "args": args}]})
    text = ("Ledger summary unchanged." if shape == "same_reply"
            else "Reconcile step %d: totals checked against batch %d." % (i, i * 7))
    r.append({"content": text, "output_tokens": 40, "thinking_tokens": 10})
json.dump({"responses": r}, open(path, "w"))
PYEOF
}

# run_shape ID SHAPE TOOL_BODY SCRIPT_STEP EXTRA_CONTEXT_DRIFT AGENT_EXTRA -> dir $T/ID
run_shape() {
    local id=$1 shape=$2 body=$3 step=$4 cdx=$5 agx=$6 d="$T/$1"
    mkdir -p "$d"; fixture "$shape" "$d/fx.json"
    start_stub "$d/fx.json" "$d" || { echo "stub did not start for $id"; return 1; }
    cat > "$d/govern.json" <<EOF
{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"},
 "telemetry": {"enabled": true, "output_file": "tele.jsonl"},
 "behavioral_sequences": {"enabled": true},
 "context_drift": {"enabled": true, "level": "advisory", "check_interval_turns": 1, $cdx
   "reality_checkpoint": {"enabled": false}},
 "agents": {"worker": {"provider": "gemini", "model": "stub-model", "api_base": "http://127.0.0.1:$STUB_PORT",
   "api_key_env": "FAKE_KEY_TL", "max_tokens": 200, "max_turns": 100, "tools_enabled": true, "tools": ["lookup"],
   $agx "system_prompt": "You reconcile ledger data."}}}
EOF
    cat > "$d/t.naab" <<EOF
use agent
use uuid
use env
fn lookup(k) {
    $body
}
main {
    agent.register_tool("lookup", lookup, {"description": "Look up a ledger value", "parameters": {"k": {"type": "string", "description": "key"}}})
    let h = agent.create("worker")
    let i = 0
    while i < 12 {
        $step
        try { let r = agent.send(h, "continue") } catch (e) { print("ERR " + string(e)) i = 12 }
        i = i + 1
    }
    print("DONE")
}
EOF
    (cd "$d" && timeout 120 "$NAAB" t.naab > out.txt 2> err.txt)
    RC=$?
    stop_stub
}

# s1 DIR -> "KIND@POS ..." for every analysed CDD turn on which S1 fired, POS
# the turn's place among the analysed turns (0 = the first send); plus the
# number of distinct tool results the model was sent.
s1() {
python3 - "$1" <<'PYEOF'
import glob, json, os, re, sys
d = sys.argv[1]
out, pos = [], 0
for line in open(os.path.join(d, "tele.jsonl")):
    if '"CDD_TURN"' not in line: continue
    try: e = json.loads(line)
    except Exception: continue
    f = e.get("fields", e)
    if f.get("event_type") != "CDD_TURN" or f.get("analyzed") != "true": continue
    if "circular" in (f.get("signals_detail") or ""):
        out.append("%s@%d" % (f.get("circular_kind") or "?", pos))
    pos += 1
res = set()
for p in glob.glob(os.path.join(d, "req_*.json")):
    for m in re.finditer(r'"functionResponse":\{"name":"lookup","response":\{"result":"([^"]*)"', open(p).read()):
        res.add(m.group(1))
print("analysed=%d results=%d fired=%s" % (pos, len(res), " ".join(out)))
PYEOF
}

SAME='return "value for " + string(k)'
echo "=== B7: S1 fires on the same tool calls getting the same results ==="

run_shape c0 same_reply "$SAME" "" "" ""; R=$(s1 "$T/c0")
if [ "$RC" = 0 ] && grep -qE 'fired=[a-z_?]+@1 ' <<<"$R"; then pass TC-00 "CONTROL: identical replies fire S1 ($R)"
else fail TC-00 "CONTROL: identical replies did not fire S1 -- every arm is UNMEASURABLE" "rc=$RC $R"; echo "=== Results: $PASS passed, $FAIL failed ==="; exit 1; fi
if grep -q 'fired=reply@1 ' <<<"$R"; then pass TC-00b "CDD_TURN names the repeat: circular_kind \"reply\""
else fail TC-00b "circular_kind does not name a reply repeat" "$R"; fi

run_shape loop loop "$SAME" "" "" ""; R=$(s1 "$T/loop")
if [ "$RC" = 0 ] && grep -q 'results=1 ' <<<"$R" && grep -q 'fired=tool_calls@4 tool_calls@5 tool_calls@6 tool_calls@7 tool_calls@8 tool_calls@9 tool_calls@10 tool_calls@11$' <<<"$R"; then
    pass TC-01 "the same call and result each turn fire S1 from the third such turn on ($R)"
else fail TC-01 "the repeated tool call did not fire S1 from its third turn" "rc=$RC $R"; fi

run_shape reread loop 'return "value " + uuid.v4()' "" "" ""; R=$(s1 "$T/reread")
if [ "$RC" = 0 ] && grep -q 'results=10 ' <<<"$R" && grep -q 'fired=$' <<<"$R"; then pass TC-02 "CONTROL: the same call whose result changes does not fire ($R)"
else fail TC-02 "a re-read whose result changed fired S1 (or the result did not change)" "rc=$RC $R"; fi

run_shape comply comply 'return "ok"' "" "" ""; R=$(s1 "$T/comply")
if [ "$RC" = 0 ] && grep -q 'results=1 ' <<<"$R" && grep -q 'fired=$' <<<"$R"; then pass TC-03 "CONTROL: different arguments each turn do not fire, though every result is the same ($R)"
else fail TC-03 "calls with different arguments fired S1" "rc=$RC $R"; fi

run_shape toolenv loop 'let h = env.get("HOME") ?? "none"
    return "value for " + string(k)' "" "" ""; R=$(s1 "$T/toolenv")
if [ "$RC" = 0 ] && grep -q 'fired=tool_calls@4 ' <<<"$R"; then pass TC-04 "the loop's tool reading the environment still fires ($R)"
else fail TC-04 "a tool's own action hid the loop" "rc=$RC $R"; fi

SINCE='"event_feed": "since_last_check",'
NOISE='let noise = env.get("NAAB_TL_VAR_" + string(i)) ?? "unset"'
run_shape since_noise loop "$SAME" "$NOISE" "$SINCE" ""; R=$(s1 "$T/since_noise")
if [ "$RC" = 0 ] && grep -q 'fired=tool_calls@4 ' <<<"$R"; then pass TC-05 "since_last_check: the script's own reads between sends do not hide the loop ($R)"
else fail TC-05 "the script's events between sends hid the agent's loop" "rc=$RC $R"; fi
run_shape since loop "$SAME" "" "$SINCE" ""; R=$(s1 "$T/since")
if [ "$RC" = 0 ] && grep -q 'fired=tool_calls@4 ' <<<"$R"; then pass TC-05c "CONTROL: the same feed without the script's reads fires ($R)"
else fail TC-05c "CONTROL: the loop did not fire on since_last_check" "rc=$RC $R"; fi

run_shape off loop "$SAME" "" "" '"context_drift_signals": {"circular_actions": false},'; R=$(s1 "$T/off")
if [ "$RC" = 0 ] && grep -q 'fired=$' <<<"$R"; then pass TC-06 "CONTROL: S1 off for the agent draws no firing ($R)"
else fail TC-06 "S1 fired though the agent switched it off" "rc=$RC $R"; fi

echo
echo "=== Results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
