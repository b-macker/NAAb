#!/usr/bin/env bash
# ============================================================
# test_tool_admissibility_gate.sh — Per-tool-call admissibility gate
#
# Group A: gate off by default — tool executes normally
# Group B: gate_tool_calls=true + low coherence — tool call denied with
#          AGENT_TOOL_BLOCKED reason=admissibility_gate
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/toolgate-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; TOTAL=0; FAILURES=""

pass() { PASS_COUNT=$((PASS_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
STUB_PID=""
cleanup() {
    [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null
    teardown_isolated_trust
    rm -rf "$TEST_TMP"
}
trap cleanup EXIT
mkdir -p "$TEST_TMP"

"$NAAB" --keygen "$TEST_TMP/test-key.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/test-key.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/test-key.pem"
export FAKE_KEY_TOOLGATE_TEST="fake-key-toolgate-test"

sign_govern() {
    (cd "$1" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
}

start_stub() {
    STUB_PORT=$(( (RANDOM % 20000) + 20000 ))
    python3 "$SCRIPT_DIR/../helpers/agent_stub.py" "$STUB_PORT" "$1" "$2" > "$2/stub.log" 2>&1 &
    STUB_PID=$!
    for _ in $(seq 1 50); do
        grep -q READY "$2/stub.log" 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}
stop_stub() { [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null; wait "$STUB_PID" 2>/dev/null; STUB_PID=""; }

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|          Per-Tool-Call Admissibility Gate Tests               |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

# ============================================================
# Group A: gate off (default) — tool executes
# ============================================================
echo -e "${CYAN}--- Group A: gate off by default ---${NC}"

WDIR="$TEST_TMP/group_a"; mkdir -p "$WDIR"
cat > "$WDIR/fixture.json" << 'EOF'
{"responses": [
  {"tool_calls": [{"name": "get_data", "args": {"query": "revenue"}}]},
  {"content": "final answer using the tool result", "output_tokens": 20}
]}
EOF
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "A-00" "stub failed to start"; exit 1; }

cat > "$WDIR/govern.json" << GOVEOF
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "telemetry": { "enabled": true, "output_file": "telemetry.jsonl" },
    "agents": {
        "tooler": {
            "provider": "gemini",
            "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": "FAKE_KEY_TOOLGATE_TEST",
            "max_tokens": 100,
            "max_turns": 20,
            "tools_enabled": true,
            "tools": ["get_data"]
        }
    }
}
GOVEOF
sign_govern "$WDIR"

cat > "$WDIR/test.naab" << 'NAABEOF'
use agent

fn get_data(query) {
    return "data for: " + string(query)
}

main {
    agent.register_tool("get_data", get_data, {
        "description": "Fetch data by query",
        "parameters": { "query": {"type": "string", "description": "what to fetch"} }
    })
    let h = agent.create("tooler")
    let r = agent.send(h, "use the tool to fetch revenue data")
    print("CONTENT=" + string(r.get("content")))
    print("TOOL_CALLS=" + string(r.get("tool_calls_made")))
    let results = r.get("tool_results")
    if results != null {
        if results.length() > 0 {
            print("TOOL_SUCCESS=" + string(results[0].get("success")))
        }
    }
}
NAABEOF

OUTPUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1) || true
stop_stub

if echo "$OUTPUT" | grep -q "TOOL_CALLS=1"; then
    pass "A-01" "tool call executed with gate off"
else
    fail "A-01" "tool did not execute" "$(echo "$OUTPUT" | head -4)"
fi
if echo "$OUTPUT" | grep -q "TOOL_SUCCESS=true"; then
    pass "A-02" "tool executed successfully (not blocked)"
else
    fail "A-02" "tool result not successful" "$(echo "$OUTPUT" | grep TOOL_ | head -2)"
fi
if grep -q "admissibility_gate" "$WDIR/telemetry.jsonl" 2>/dev/null; then
    fail "A-03" "gate fired although disabled"
else
    pass "A-03" "no admissibility_gate telemetry when gate is off"
fi

# ============================================================
# Group B: gate on + low coherence — tool call denied
# ============================================================
echo -e "${CYAN}--- Group B: gate denies tool at low coherence ---${NC}"

WDIR="$TEST_TMP/group_b"; mkdir -p "$WDIR"
cat > "$WDIR/fixture.json" << 'EOF'
{"responses": [
  {"content": "not json response one for coherence decay", "output_tokens": 30},
  {"content": "not json response two for coherence decay", "output_tokens": 30},
  {"content": "not json response three for coherence decay", "output_tokens": 30},
  {"content": "not json response four for coherence decay", "output_tokens": 30},
  {"tool_calls": [{"name": "get_data", "args": {"query": "revenue"}}]},
  {"content": "final text after denied tool", "output_tokens": 20}
]}
EOF
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "B-00" "stub failed to start"; exit 1; }

cat > "$WDIR/govern.json" << GOVEOF
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "telemetry": { "enabled": true, "output_file": "telemetry.jsonl" },
    "behavioral_sequences": { "enabled": true },
    "context_drift": { "enabled": true, "check_interval_turns": 1 },
    "circuit_breaker": {
        "enabled": true,
        "output_admissibility": {
            "enabled": true,
            "threshold": 0.95,
            "action": "quarantine",
            "gate_tool_calls": true
        }
    },
    "agents": {
        "tooler": {
            "provider": "gemini",
            "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": "FAKE_KEY_TOOLGATE_TEST",
            "max_tokens": 100,
            "max_turns": 20,
            "response_format": "json",
            "tools_enabled": true,
            "tools": ["get_data"]
        }
    }
}
GOVEOF
sign_govern "$WDIR"

cat > "$WDIR/test.naab" << 'NAABEOF'
use agent
use file

fn get_data(query) {
    file.write("tool_side_effect.txt", "tool ran")
    return "data for: " + string(query)
}

main {
    agent.register_tool("get_data", get_data, {
        "description": "Fetch data by query",
        "parameters": { "query": {"type": "string", "description": "what to fetch"} }
    })
    let h = agent.create("tooler")
    // Warm-up: drive coherence down with contract-failing turns
    for i in 0..4 {
        try {
            let w = agent.send(h, "warm up turn " + string(i))
        } catch (e) {
            print("WARMUP_ERR")
        }
    }
    // Now the tool turn — the gate should deny execution
    let r = agent.send(h, "use the tool to fetch revenue data")
    let results = r.get("tool_results")
    if results != null {
        if results.length() > 0 {
            print("TOOL_ERROR=" + string(results[0].get("error")))
            print("TOOL_SUCCESS=" + string(results[0].get("success")))
        } else {
            print("NO_TOOL_RESULTS")
        }
    } else {
        print("NO_TOOL_RESULTS")
    }
}
NAABEOF

OUTPUT=$(cd "$WDIR" && timeout 90s "$NAAB" test.naab 2>&1) || true
stop_stub

if grep -q '"reason": *"admissibility_gate"\|admissibility_gate' "$WDIR/telemetry.jsonl" 2>/dev/null; then
    pass "B-01" "AGENT_TOOL_BLOCKED with reason=admissibility_gate emitted"
else
    fail "B-01" "gate did not fire" "$(echo "$OUTPUT" | tail -4)"
fi
if echo "$OUTPUT" | grep -q "TOOL_ERROR=admissibility_gate"; then
    pass "B-02" "tool result marked blocked by admissibility gate"
else
    fail "B-02" "tool result not marked as gate-blocked" "$(echo "$OUTPUT" | grep -E 'TOOL_|NO_TOOL' | head -2)"
fi
if [ ! -f "$WDIR/tool_side_effect.txt" ]; then
    pass "B-03" "tool function never executed (no side effect)"
else
    fail "B-03" "tool side effect file exists — tool ran despite gate"
fi

# ============================================================
echo ""
echo -e "${CYAN}==============================================${NC}"
echo -e "  Total: $TOTAL  ${GREEN}Pass: $PASS_COUNT${NC}  ${RED}Fail: $FAIL_COUNT${NC}  ${YELLOW}Skip: $SKIP_COUNT${NC}"
[ -n "$FAILURES" ] && echo -e "${RED}Failures:$FAILURES${NC}"
echo -e "${CYAN}==============================================${NC}"
[ $FAIL_COUNT -eq 0 ]
