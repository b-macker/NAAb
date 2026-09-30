#!/usr/bin/env bash
# ============================================================
# test_vocab_agent_events.sh — S5 counts only the agent's own actions
#
# S5 (vocabulary_contraction) compares the variety of event types in the early
# and recent halves of its window. Its types used to come from EVERY event in
# the agent's turn bucket, and under the default feed turn 0's bucket holds all
# the orchestration script did before the first send. That startup variety set
# S5's frozen reference entropy, so a tool-less agent that only sends and
# receives "contracted" from it and paid on every turn once the window filled.
# entropy_baseline_adaptive (#265) trims the tail of long runs but cannot help
# a short-lived agent: turn 0 stays in the early half it re-derives from.
# Measured live (repo-sentinel round 6): reference entropy 2.113 on turns 6-8,
# the reviewer quarantined at turn 8 in 3 of 3 clean runs.
#
# thresholds.vocab_contraction_agent_events_only (default true) restricts S5 to
# AGENT_SEND / AGENT_RESPONSE / TOOL_*.
#
#   VA-01  a sentinel-shaped startup before a tool-less agent: S5 never fires
#   VA-02  CONTROL: the same arm with the flag false fires, under the shipped
#          adaptive default -- the fixture reproduces the live defect, so VA-01
#          is not passing on a fixture that could never fire
#   VA-03  POSITIVE CONTROL: an agent that uses a tool on its first turns and
#          then stops still trips S5 on the default config -- without it, VA-01
#          passes for an engine where S5 is simply dead
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"
_SYSTMP="${TMPDIR:-/tmp}"
TEST_TMP="${_SYSTMP}/vocab-agent-events-$$"

PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo "  PASS [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo "  SKIP [$1] $2"; }

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_vocab_agent_events.sh"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
STUB_PID=""
cleanup() {
    [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null
    teardown_isolated_trust
    rm -rf "${TEST_TMP:?}"
}
trap cleanup EXIT
mkdir -p "$TEST_TMP"
export FAKE_KEY_VA="fake-key-vocab-agent-events"
source "$SCRIPT_DIR/../helpers/stub_launch.sh"

TURNS=14

# $1=dir $2=tool_turns (0 = tool-less)
gen_fixture() {
python3 - "$1" "$2" "$TURNS" <<'EOF'
import json, sys
path, tool_turns, turns = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
r = []
for i in range(turns + 5):
    if i < tool_turns:
        r.append({"tool_calls": [{"name": "peek_env", "args": {"k": "HOME"}}]})
    r.append({"content": "Review step %d: findings recorded for module %d." % (i, i),
              "output_tokens": 40, "thinking_tokens": 15})
json.dump({"responses": r}, open(path, "w"))
EOF
}

# $1=arm  $2=tool_turns  $3=extra thresholds JSON (or "")
run_arm() {
    local d="$TEST_TMP/$1"; mkdir -p "$d"
    gen_fixture "$d/fixture.json" "$2"
    STUB_PID=""; start_stub "$d/fixture.json" "$d" || return 1
    local tools='' th=''
    [ "$2" -gt 0 ] && tools=', "tools_enabled": true, "tools": ["peek_env"]'
    [ -n "$3" ] && th=", \"thresholds\": { $3 }"
    cat > "$d/govern.json" <<EOF
{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl" },
  "behavioral_sequences": { "enabled": true },
  "context_drift": { "enabled": true, "level": "advisory", "check_interval_turns": 1,
    "reality_checkpoint": { "enabled": false }$th },
  "circuit_breaker": { "enabled": true, "critical_threshold": 0.99 },
  "agents": { "reviewer": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT", "api_key_env": "FAKE_KEY_VA",
      "max_tokens": 200, "max_turns": 60$tools,
      "system_prompt": "You review code and record findings." } } }
EOF
    # repo-sentinel's shape: the script does varied work BEFORE the first send
    # (git via process.run, file writes, env, encoding), then the agent loops.
    cat > "$d/t.naab" <<EOF
use agent
use env
use crypto
use file
use process
fn peek_env(k) { return env.get(string(k)) ?? "unset" }
main {
    let g = process.run("python3", ["-c", "print(42)"])
    let _a = env.get("HOME") ?? "x"
    let _b = crypto.base64_encode("d")
    file.write("stage1.txt", "hotspots")
    let _c = file.read("stage1.txt")
    agent.register_tool("peek_env", peek_env, {
        "description": "Read an environment variable",
        "parameters": { "k": {"type": "string", "description": "var name"} }
    })
    let h = agent.create("reviewer")
    let i = 0
    while i < $TURNS {
        try { let r = agent.send(h, "review the next module") } catch (e) { i = $TURNS }
        i = i + 1
    }
    print("DONE")
}
EOF
    (cd "$d" && timeout 300 "$NAAB" t.naab > out.txt 2>&1) || true
    kill "$STUB_PID" 2>/dev/null; STUB_PID=""
    [ -f "$d/tele.jsonl" ]
}

# Turns on which S5 fired (signals_detail covers absorbed firings too).
s5_turns() {
python3 - "$1" <<'EOF'
import json, sys
out = []
for l in open(sys.argv[1], encoding="utf-8", errors="replace"):
    if '"CDD_TURN"' not in l: continue
    try: e = json.loads(l)
    except ValueError: continue
    if e.get("analyzed") != "true": continue
    if "vocab_contraction" in (e.get("signals_detail") or "") + (e.get("penalties_detail") or ""):
        out.append(int(e["turn"]))
print(" ".join(map(str, sorted(out))))
EOF
}

echo "=== S5 counts only the agent's own actions ==="

run_arm startup_default 0 "" || { skip "VA-01" "arm produced no telemetry (UNMEASURABLE)"; }
run_arm startup_legacy 0 '"vocab_contraction_agent_events_only": false' || { skip "VA-02" "arm produced no telemetry (UNMEASURABLE)"; }
run_arm tools_then_stop 6 "" || { skip "VA-03" "arm produced no telemetry (UNMEASURABLE)"; }

if [ -f "$TEST_TMP/startup_legacy/tele.jsonl" ]; then
    L="$(s5_turns "$TEST_TMP/startup_legacy/tele.jsonl")"
    if [ -n "$L" ]; then
        pass "VA-02" "control: with script events counted, the startup artifact fires S5 (turns $L)"
    else
        fail "VA-02" "the legacy arm never fired -- the fixture does not reproduce the defect, VA-01 is vacuous"
    fi
fi
if [ -f "$TEST_TMP/startup_default/tele.jsonl" ]; then
    D="$(s5_turns "$TEST_TMP/startup_default/tele.jsonl")"
    if [ -z "$D" ]; then
        pass "VA-01" "default: script startup work no longer trips S5 for a tool-less agent"
    else
        fail "VA-01" "S5 still fires on the startup artifact" "turns $D"
    fi
fi
if [ -f "$TEST_TMP/tools_then_stop/tele.jsonl" ]; then
    TC=$(grep -c '"event_type":"AGENT_TOOL_CALL"' "$TEST_TMP/tools_then_stop/tele.jsonl" || true)
    T="$(s5_turns "$TEST_TMP/tools_then_stop/tele.jsonl")"
    if [ "${TC:-0}" -eq 0 ]; then
        fail "VA-03" "no tool calls executed -- the positive control cannot run" "$(tail -3 "$TEST_TMP/tools_then_stop/out.txt")"
    elif [ -n "$T" ]; then
        pass "VA-03" "positive control: an agent that stops using its tools still trips S5 (turns $T, $TC tool calls)"
    else
        fail "VA-03" "S5 never fired for an agent that narrowed its own actions -- the signal is dead"
    fi
fi

echo ""
echo "Results: $PASS_COUNT passed, $FAIL_COUNT failed, $SKIP_COUNT skipped"
[ "$FAIL_COUNT" -eq 0 ]
