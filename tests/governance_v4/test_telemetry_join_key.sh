#!/usr/bin/env bash
# ============================================================
# test_telemetry_join_key.sh — AGENT_RESPONSE ↔ CDD_TURN join key
#
# AGENT_RESPONSE carries the model, tokens, latency and key used. CDD_TURN
# carries the coherence and signals scored from that same response. Joining them
# had no key, so a reader matched them positionally.
#
# The obvious fix -- put the turn on AGENT_RESPONSE -- does not work, and the
# way it fails is the point of this suite. AGENT_RESPONSE is emitted ONCE, before
# the tool loop. The loop then advances tracker.turns once per round-trip, and a
# further increment follows it, so CDD_TURN reports:
#
#     request_turn + tool_round_trips + 1
#
# A field emitted at response time can only guess that. The first version guessed
# current_turn + 1 and was correct for tool-FREE agents only; with two round-trips
# it read 1 against CDD_TURN's 3. It shipped because it was verified against a
# tool-free scenario -- a join that looks valid and is silently wrong, which is
# strictly worse than the positional join it replaced, because a positional
# mismatch at least shows up as a count mismatch.
#
# Both events now carry turn_at_request, assigned at the single point the
# response arrives and read unchanged by both emitters, so neither has to predict
# anything.
#
# JK-01 is the case the original verification could not have caught. JK-02 is the
# control that the tool-free path (which the broken version DID satisfy) still
# works -- without it, a fix that broke the common path would pass.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_telemetry_join_key.sh"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/joinkey-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""

pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
source "$SCRIPT_DIR/../helpers/stub_launch.sh"
cleanup() { stop_stub; teardown_isolated_trust; rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"

if [ ! -x "$NAAB" ]; then
    skip "JK-00" "build/naab-lang not found"
    echo "  Results: 0 passed, 0 failed, 1 skipped"
    exit 0
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  Telemetry join key: AGENT_RESPONSE <-> CDD_TURN               |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

# $1 = workdir, $2 = fixture JSON body, $3 = "tools" | "notools"
run_case() {
    local w="$1" fixture="$2" mode="$3"
    mkdir -p "$w"
    printf '%s\n' "$fixture" > "$w/fixture.json"
    start_stub "$w/fixture.json" "$w" || return 1

    local tool_cfg="" tool_src=""
    if [ "$mode" = "tools" ]; then
        tool_cfg='"tools_enabled": true, "tools": ["get_data"],'
        tool_src='agent.register_tool("get_data", get_data, {"description":"d","parameters":{"query":{"type":"string","description":"q"}}})'
    fi

    cat > "$w/govern.json" <<GOVEOF
{ "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "telemetry.jsonl" },
  "behavioral_sequences": { "enabled": true },
  "context_drift": { "enabled": true, "check_interval_turns": 1 },
  "agents": { "a": {
      "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT",
      "api_key_env": "FAKE_JOINKEY",
      "max_tokens": 100, "max_turns": 20, $tool_cfg
      "system_prompt": "Use tools to fetch data and answer." } } }
GOVEOF

    cat > "$w/t.naab" <<NAABEOF
use agent
fn get_data(query) { return "data for: " + string(query) }
main {
    $tool_src
    let h = agent.create("a")
    let r = agent.send(h, "do the work")
    print("OK")
}
NAABEOF
    (cd "$w" && FAKE_JOINKEY=x "$NAAB" t.naab > out.txt 2>&1)
    stop_stub
}

# --- JK-01: tool path (two round-trips) -----------------------------------
# The case the original tool-free verification structurally could not catch.
run_case "$TEST_TMP/tools" \
'{"responses": [
  {"tool_calls": [{"name": "get_data", "args": {"query": "a"}}]},
  {"tool_calls": [{"name": "get_data", "args": {"query": "b"}}]},
  {"content": "final answer using both tool results here", "output_tokens": 20}
]}' "tools"

JK1=$(python3 - "$TEST_TMP/tools/telemetry.jsonl" 2>/dev/null <<'PY'
import json,sys
try:
    rows=[json.loads(l) for l in open(sys.argv[1]) if l.strip()]
except Exception:
    print("NOFILE"); raise SystemExit
resp=[r.get("turn_at_request") for r in rows if r.get("event_type")=="AGENT_RESPONSE"]
cdd =[r.get("turn_at_request") for r in rows if r.get("event_type")=="CDD_TURN"]
cddturn=[r.get("turn") for r in rows if r.get("event_type")=="CDD_TURN"]
if not resp or not cdd: print("MISSING"); raise SystemExit
# the join must hold, AND CDD's own turn must still show the post-loop count
print("OK" if resp==cdd and cddturn and cddturn[0]!=resp[0] else "MISMATCH", resp, cdd, cddturn)
PY
)
case "$JK1" in
    OK*)      pass "JK-01" "tool path joins on turn_at_request ($JK1)" ;;
    MISMATCH*) fail "JK-01" "tool path does not join" "$JK1" ;;
    *)        fail "JK-01" "no telemetry produced on the tool path" "$JK1" ;;
esac

# --- JK-02: tool-free control ---------------------------------------------
# The broken version satisfied this case; it must keep working.
run_case "$TEST_TMP/notools" \
'{"responses": [{"content": "a plain answer with no tools involved", "output_tokens": 20}]}' "notools"

JK2=$(python3 - "$TEST_TMP/notools/telemetry.jsonl" 2>/dev/null <<'PY'
import json,sys
try:
    rows=[json.loads(l) for l in open(sys.argv[1]) if l.strip()]
except Exception:
    print("NOFILE"); raise SystemExit
resp=[r.get("turn_at_request") for r in rows if r.get("event_type")=="AGENT_RESPONSE"]
cdd =[r.get("turn_at_request") for r in rows if r.get("event_type")=="CDD_TURN"]
if not resp or not cdd: print("MISSING"); raise SystemExit
print("OK" if resp==cdd else "MISMATCH", resp, cdd)
PY
)
case "$JK2" in
    OK*)      pass "JK-02" "tool-free path still joins (control) ($JK2)" ;;
    MISMATCH*) fail "JK-02" "tool-free path regressed" "$JK2" ;;
    *)        fail "JK-02" "no telemetry produced on the tool-free path" "$JK2" ;;
esac

# --- JK-03: the key must never be the unassigned sentinel ------------------
if echo "$JK1 $JK2" | grep -q "'-1'"; then
    fail "JK-03" "turn_at_request emitted as the -1 sentinel" \
         "assigned only inside a guard that did not run"
else
    pass "JK-03" "join key is never the unassigned sentinel"
fi

echo ""
echo -e "${CYAN}==============================================================${NC}"
echo -e "  Results: ${GREEN}$PASS_COUNT passed${NC}, ${RED}$FAIL_COUNT failed${NC}, ${YELLOW}$SKIP_COUNT skipped${NC}"
echo -e "${CYAN}==============================================================${NC}"
[ "$FAIL_COUNT" -eq 0 ] || { echo -e "${RED}Failures:${NC}$FAILURES"; exit 1; }
exit 0
