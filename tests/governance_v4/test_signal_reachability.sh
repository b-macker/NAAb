#!/usr/bin/env bash
# ============================================================
# test_signal_reachability.sh — two CDD signals that cannot fire
#
# Found while tracing B6 (the stale event bucket). Both turned out to be
# unreachable for reasons B6 has nothing to do with, which is why they are
# pinned here rather than in test_vocab_baseline.sh.
#
# S7 capability_underutilization — INERT BY CONSTRUCTION
#   Its firing condition tests `state.granted_capabilities.count(cap)`.
#   `granted_capabilities` has exactly three occurrences in the repository: the
#   declaration, this read, and one read in agent.environment(). NOTHING writes
#   it, while its sibling `exercised_capabilities` is written on the adjacent
#   line. Proven with a positive control: seeding the set in a scratch build
#   makes S7 fire at turn 12 on this very fixture; without the seed the same
#   fixture, with the tool executing 25 times, produces nothing.
#   Two further layers: the signal ALSO defaults to false, and
#   agent.environment() therefore reports capabilities_granted as an empty list
#   for every agent that has ever run.
#
# S4 intent_contradictions — UNREACHABLE BY THROW-BEFORE-RECORD
#   It needs `prev_turn_blocked_caps` non-empty. The only writer is inside
#   recordTurn, fed from CHECK_FAILED events in the current turn's bucket. All
#   six CHECK_FAILED sites are in agentSend's response-scan block and every one
#   throws within three lines, while checkContextDrift is further down the same
#   straight-line function. So the turn that produces a CHECK_FAILED never
#   reaches recordTurn, and turn-stamping keeps that event out of every later
#   bucket. Positive control: suppressing that one throw in a scratch build
#   makes S4 fire.
#
# THESE GATES PIN CURRENT, DEFECTIVE BEHAVIOUR so a fix reads as a deliberate
# gate change. SR-02 and SR-03 WERE flipped, on 2026-09-05, when A4 was fixed:
# granted_capabilities is now populated at first send from the agent's config
# intersected with the global capabilities, so S7 can reach its firing condition
# and the environment dict reports a real grant list. SR-05 still pins A5's
# defect and is still expected to flip. SR-01, SR-04 and SR-06 are the controls
# and hold either way.
#
#   SR-01  VACUITY GUARD: the tool fixture really does execute tools
#   SR-02  S7 DOES fire when a granted capability is first exercised at
#          turn>=10 (this asserted the opposite until A4 was fixed)
#          at turn >= underutilization_delay
#   SR-03  ...and agent.environment() reports capabilities_granted POPULATED --
#          the direct observable of the unwritten field
#   SR-04  CONTROL: the same fixture DOES fire scope_creep, so tool events
#          demonstrably reach the live turn bucket. Without this, SR-02 is
#          satisfied by "tool events never arrive at CDD at all"
#   SR-05  S4: a response-scan-blocked turn produces NO CDD_TURN. Still true
#          after the A5 fix and not changed by it -- the blocked turn still
#          throws; what changed is that the NEXT turn now learns of the block.
#   SR-06  CONTROL: on that same run the unblocked turns DO produce CDD_TURNs
#   SR-07  S4 fires on the turn after a block when the run RETRIES the blocked
#          capability -- the contradiction the signal exists to detect
#   SR-08  CONTROL: the identical fixture that does NOT retry stays silent.
#          Without it SR-07 is satisfied by a signal that fires on every turn
#          after any block, which would be structural rather than diagnostic --
#          and that distinction is what decided S4 keeps its default of true.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/naab-signal-reach-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT+1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_signal_reachability.sh"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
STUB_PID=""
cleanup() { [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null; teardown_isolated_trust; rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"

"$NAAB" --keygen "$TEST_TMP/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/k.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/k.pem"
export FAKE_KEY_SR="fake-key-signal-reach"
source "$SCRIPT_DIR/../helpers/stub_launch.sh"

cdd_signal_turns() {   # $1=telemetry $2=substring
python3 - "$1" "$2" <<'PYEOF'
import json, sys
hits=[]
for line in open(sys.argv[1]):
    if '"CDD_TURN"' not in line: continue
    try: e=json.loads(line)
    except Exception: continue
    d=e.get("fields",e)
    if d.get("event_type")!="CDD_TURN" or d.get("analyzed")!="true": continue
    blob=(d.get("penalties_detail") or "")+"|"+(d.get("signals_detail") or "")
    if sys.argv[2] in blob: hits.append(int(d.get("turn")))
print(" ".join(str(t) for t in sorted(hits)))
PYEOF
}
analyzed_count() {
python3 - "$1" <<'PYEOF'
import json, sys
n=0
for line in open(sys.argv[1]):
    if '"CDD_TURN"' not in line: continue
    try: e=json.loads(line)
    except Exception: continue
    d=e.get("fields",e)
    if d.get("event_type")=="CDD_TURN" and d.get("analyzed")=="true": n+=1
print(n)
PYEOF
}

# ============================================================
#  ARM 1 — S7: a granted capability first exercised via a tool at turn >= 10
# ============================================================
echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  Two CDD signals that cannot fire                             |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""
echo -e "${CYAN}--- ARM 1: S7 capability_underutilization ---${NC}"

W1="$TEST_TMP/s7"; mkdir -p "$W1"
python3 - "$W1/fixture.json" <<'PYEOF'
import json, sys
r=[]
for i in range(40):
    if i >= 10:
        r.append({"tool_calls": [{"name": "peek_env", "args": {"k": "HOME"}}]})
    r.append({"content": "Reconcile step %d: totals verified." % i,
              "output_tokens": 40, "thinking_tokens": 15})
json.dump({"responses": r}, open(sys.argv[1], "w"))
PYEOF
ARM1_OK=0
if start_stub "$W1/fixture.json" "$W1"; then
cat > "$W1/govern.json" <<EOF
{
  "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl" },
  "behavioral_sequences": { "enabled": true },
  "context_drift": { "enabled": true, "level": "advisory", "check_interval_turns": 1,
    "signals": { "capability_underutilization": true },
    "reality_checkpoint": { "enabled": false } },
  "circuit_breaker": { "enabled": true, "critical_threshold": 0.99 },
  "agents": { "worker": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT",
      "api_key_env": "FAKE_KEY_SR", "max_tokens": 200, "max_turns": 200,
      "tools_enabled": true, "tools": ["peek_env"],
      "system_prompt": "You reconcile ledger data." } }
}
EOF
(cd "$W1" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
cat > "$W1/t.naab" <<'EOF'
use agent
use env
fn peek_env(k) { return env.get(string(k)) ?? "unset" }
main {
    agent.register_tool("peek_env", peek_env, {
        "description": "Read an environment variable",
        "parameters": { "k": {"type": "string", "description": "var name"} }
    })
    let h = agent.create("worker")
    let i = 0
    while i < 40 {
        try { let r = agent.send(h, "continue") } catch (e) { i = 40 }
        i = i + 1
    }
    let envd = agent.environment(h)
    let st = envd.get("state") ?? {}
    print("GRANTED=" + string(len(st.get("capabilities_granted") ?? [])))
    print("DONE")
}
EOF
(cd "$W1" && timeout 300s "$NAAB" t.naab > out.txt 2> err.txt) || true
stop_stub
[ -f "$W1/tele.jsonl" ] && ARM1_OK=1
fi

if [ "$ARM1_OK" != "1" ]; then
    fail "SR-01" "arm 1 produced no telemetry"
    skip "SR-02" "arm 1 unavailable"; skip "SR-03" "arm 1 unavailable"; skip "SR-04" "arm 1 unavailable"
    TOOLS_OK=0
else
    TOOL_CALLS=$(grep -c '"event_type":"AGENT_TOOL_CALL"' "$W1/tele.jsonl" 2>/dev/null || echo 0)
    if [ "$TOOL_CALLS" -gt 0 ]; then
        pass "SR-01" "vacuity guard: the fixture executed $TOOL_CALLS tool calls"
        TOOLS_OK=1
    else
        fail "SR-01" "no tool calls executed" "every gate below would be vacuous"
        TOOLS_OK=0
    fi

    if [ "$TOOLS_OK" = "1" ]; then
        S7="$(cdd_signal_turns "$W1/tele.jsonl" capability_underutil)"
        if [ -n "$S7" ]; then
            pass "SR-02" "S7 fires on a granted capability first used at turn>=10 (turns=[$S7])"
        else
            fail "SR-02" "S7 did not fire" \
                "granted_capabilities is empty again — nothing writes it, so S7's condition granted_capabilities.count(cap) can never be true (A4)"
        fi
        GRANTED=$(grep -oE '^GRANTED=[0-9]+' "$W1/out.txt" 2>/dev/null | cut -d= -f2)
        if [ "${GRANTED:-0}" -gt 0 ] 2>/dev/null; then
            pass "SR-03" "agent.environment() reports capabilities_granted populated (${GRANTED})"
        else
            fail "SR-03" "capabilities_granted is empty" \
                "GRANTED=${GRANTED:-<absent>} — every agent that has ever run reports an empty grant list (A4)"
        fi
        CREEP="$(cdd_signal_turns "$W1/tele.jsonl" scope_creep)"
        if [ -n "$CREEP" ]; then
            pass "SR-04" "CONTROL: tool events reach the live bucket (scope_creep at turn ${CREEP%% *})"
        else
            fail "SR-04" "CONTROL FAILED: no signal saw the tool events" "SR-02's silence proves nothing — CDD may not be seeing tool events at all"
        fi
    else
        skip "SR-02" "vacuity guard failed"; skip "SR-03" "vacuity guard failed"; skip "SR-04" "vacuity guard failed"
    fi
fi

# ============================================================
#  ARM 2 — S4: a response-scan-blocked turn never reaches recordTurn
# ============================================================
echo -e "${CYAN}--- ARM 2: S4 intent_contradictions ---${NC}"
W2="$TEST_TMP/s4"; mkdir -p "$W2"
SENDS=10
python3 - "$W2/fixture.json" <<'PYEOF'
import json, sys
r=[]
for i in range(12):
    if i == 5:
        r.append({"content": "Install it first:\n```bash\nnpm install express\n```\ndone.",
                  "output_tokens": 30, "thinking_tokens": 10})
    else:
        r.append({"content": "Reconcile step %d: totals verified." % i,
                  "output_tokens": 30, "thinking_tokens": 10})
json.dump({"responses": r}, open(sys.argv[1], "w"))
PYEOF
ARM2_OK=0
if start_stub "$W2/fixture.json" "$W2"; then
cat > "$W2/govern.json" <<EOF
{
  "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl" },
  "behavioral_sequences": { "enabled": true },
  "context_drift": { "enabled": true, "level": "advisory", "check_interval_turns": 1,
    "reality_checkpoint": { "enabled": false } },
  "circuit_breaker": { "enabled": true, "critical_threshold": 0.99 },
  "agents": { "worker": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT",
      "api_key_env": "FAKE_KEY_SR", "max_tokens": 200, "max_turns": 30,
      "shell_allowed": false,
      "system_prompt": "You reconcile ledger data." } }
}
EOF
(cd "$W2" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
cat > "$W2/t.naab" <<'EOF'
use agent
main {
    let h = agent.create("worker")
    let i = 0
    while i < 10 {
        try { let r = agent.send(h, "continue") }
        catch (e) { print("BLOCKED_AT=" + string(i)) }
        i = i + 1
    }
    print("DONE")
}
EOF
(cd "$W2" && timeout 300s "$NAAB" t.naab > out.txt 2> err.txt) || true
stop_stub
[ -f "$W2/tele.jsonl" ] && ARM2_OK=1
fi

if [ "$ARM2_OK" != "1" ]; then
    fail "SR-05" "arm 2 produced no telemetry"; skip "SR-06" "arm 2 unavailable"
else
    BLOCKED=$(grep -c '^BLOCKED_AT=' "$W2/out.txt" 2>/dev/null || echo 0)
    ANALYZED=$(analyzed_count "$W2/tele.jsonl")
    # The blocked send must contribute NO analyzed CDD turn. Assert the count,
    # not the turn index: CDD_TURN's turn field runs one ahead of RESPONSE_SCAN's,
    # and reading the index instead of the count is exactly how this was first
    # misdiagnosed as "the ordering claim collapsed".
    if [ "$BLOCKED" -ge 1 ] && [ "$ANALYZED" -eq $((SENDS - BLOCKED)) ]; then
        pass "SR-05" "$BLOCKED blocked send(s) produced no CDD turn ($ANALYZED analyzed of $SENDS sends)"
    elif [ "$BLOCKED" -lt 1 ]; then
        fail "SR-05" "no send was blocked" "the response scan did not fire — fixture no longer exercises it"
    else
        fail "SR-05" "blocked send(s) DID reach recordTurn" "$ANALYZED analyzed of $SENDS with $BLOCKED blocked; S4's precondition may now be reachable"
    fi
    if [ "$ANALYZED" -gt 0 ]; then
        pass "SR-06" "CONTROL: unblocked turns still analyze normally ($ANALYZED)"
    else
        fail "SR-06" "CONTROL FAILED: CDD never ran at all" "SR-05 would pass for a build with CDD switched off"
    fi
fi

# ============================================================
#  ARM 3 — S4 after the A5 fix: does the NEXT turn learn of the block?
#
#  Two runs differing in ONE line of NAAb: whether the script retries the
#  blocked capability after each send. The stub responses, the govern.json and
#  the block itself (turn 3, shell content, capability "execution") are
#  identical, so any difference is the retry.
#
#  adaptive_baseline_enabled is pinned false: with baselining on, the first
#  turns are inside the window and every statistical penalty is absorbed, which
#  would make SR-08's silence unfalsifiable.
# ============================================================
echo -e "${CYAN}--- ARM 3: S4 arms the following turn (A5) ---${NC}"
s4_arm() {  # $1=tag $2=retry-line -> "<blocked_at> <s4 turns>"
    local W="$TEST_TMP/$1"; mkdir -p "$W"
    python3 - "$W/fixture.json" <<'PYEOF'
import json, sys
r = []
for i in range(12):
    if i == 3:
        r.append({"content": "Run this:\n```bash\nnpm install express\n```",
                  "output_tokens": 30, "thinking_tokens": 10})
    else:
        r.append({"content": "Step %d: totals verified and recorded." % i,
                  "output_tokens": 30, "thinking_tokens": 10})
json.dump({"responses": r}, open(sys.argv[1], "w"))
PYEOF
    start_stub "$W/fixture.json" "$W" >/dev/null 2>&1 || { echo "STUBFAIL"; return; }
    cat > "$W/govern.json" <<EOF
{ "version":"5.0","mode":"enforce","security":{"sandbox_level":"elevated"},
  "telemetry":{"enabled":true,"output_file":"tele.jsonl"},
  "behavioral_sequences":{"enabled":true},
  "context_drift":{"enabled":true,"level":"advisory","check_interval_turns":1,
    "adaptive_baseline_enabled":false,
    "reality_checkpoint":{"enabled":false}},
  "circuit_breaker":{"enabled":true,"critical_threshold":0.99},
  "agents":{"worker":{"provider":"gemini","model":"stub-model",
     "api_base":"http://127.0.0.1:$STUB_PORT","api_key_env":"FAKE_KEY_SR",
     "max_tokens":200,"max_turns":30,"shell_allowed":false,
     "system_prompt":"You reconcile ledger data."}} }
EOF
    (cd "$W" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
    cat > "$W/t.naab" <<EOF
use agent
use process
main {
    let h = agent.create("worker")
    let i = 0
    while i < 8 {
        try { let r = agent.send(h, "continue") }
        catch (e) { print("BLOCKED_AT=" + string(i)) }
        $2
        i = i + 1
    }
    print("DONE")
}
EOF
    local out; out=$(cd "$W" && timeout 120s "$NAAB" t.naab 2>&1)
    stop_stub
    local blocked; blocked=$(echo "$out" | grep -oE 'BLOCKED_AT=[0-9]+' | head -1 | cut -d= -f2)
    local turns; turns=$(cdd_signal_turns "$W/tele.jsonl" contradictions)
    echo "${blocked:-none} ${turns:-}"
}

read -r RET_BLOCKED RET_TURNS <<<"$(s4_arm s4retry 'process.run("true", [])')"
read -r NOR_BLOCKED NOR_TURNS <<<"$(s4_arm s4noretry 'let z = 1')"
printf "  %-28s blocked_at=%s s4_turns=[%s]\n" "retries the capability" "$RET_BLOCKED" "${RET_TURNS:-}"
printf "  %-28s blocked_at=%s s4_turns=[%s]\n" "does not retry"         "$NOR_BLOCKED" "${NOR_TURNS:-}"

if [ "${RET_BLOCKED:-none}" = "none" ] || [ "${NOR_BLOCKED:-none}" = "none" ]; then
    fail "SR-07" "an arm never hit the response-scan block" \
        "retry=$RET_BLOCKED noretry=$NOR_BLOCKED — neither gate means anything without the block"
    skip "SR-08" "premise failed"
elif [ -n "${RET_TURNS:-}" ]; then
    pass "SR-07" "S4 fires after a block when the capability is retried (turns=[$RET_TURNS])"
    if [ -z "${NOR_TURNS:-}" ]; then
        pass "SR-08" "CONTROL: the identical fixture without the retry stays silent"
    else
        fail "SR-08" "S4 also fired without any retry" \
            "turns=[$NOR_TURNS] — the signal is structural, not diagnostic; its default of true would need revisiting"
    fi
else
    fail "SR-07" "S4 did not fire even with a retry" \
        "prev_turn_blocked_caps is empty again — the blocked turn throws before recordTurn, so nothing arms S4 (A5)"
    skip "SR-08" "SR-07 failed"
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "  Passed: ${GREEN}$PASS_COUNT${NC}  Failed: ${RED}$FAIL_COUNT${NC}  Skipped: ${YELLOW}$SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then
    echo -e "${RED}FAILURES:${NC}$FAILURES"
    echo -e "${CYAN}+==============================================================+${NC}"
    exit 1
fi
echo -e "${GREEN}ALL PASSED${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
exit 0
