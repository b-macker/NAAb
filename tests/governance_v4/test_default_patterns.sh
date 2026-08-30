#!/usr/bin/env bash
# ============================================================
# test_default_patterns.sh — the built-in BSD patterns, and the fact that
#                            most of them had never fired
#
# WHAT WAS WRONG
#
# matchesStep accepts a step name when normalizeEventTypeName(matcher) — which
# lowercases and turns '_' into '.' — equals eventTypeToString(event.type).
# Those two disagree for most event types: eventTypeToString(TOOL_CALL) is
# "tool_call", while the enum spelling normalizes to "tool.call". 17 of 24 type
# names are therefore silent no-ops in the UPPERCASE_UNDERSCORE form that
# CLAUDE.md documents as accepted — and its example, AGENT_SEND, happens to be
# one of the seven that work, which is why the claim read as verified.
#
# buildDefaultPatterns() wrote its steps in that enum style, and makeStep stores
# them verbatim, so the built-ins took the same dead path: 10 of 16 dead at one
# or more steps, 1 degraded, 5 alive. The five survivors are exactly the ones
# somebody happened to write in lowercase. Dead included credential_harvesting,
# every tool_* pattern and both taint patterns.
#
# WHY THE FIX SHIPS IN OBSERVE MODE
#
# Six of the ten dead patterns are SOFT — they block with exit 3. Correcting the
# names is therefore not repairing working behaviour, it is the first time these
# patterns have ever fired, and switching six blockers on during an upgrade is
# not something to do silently. credential_harvesting is the clearest case: an
# env var matching *key*|*token*|*secret* followed by a network call within 5
# events is the intended detection AND what every legitimate API client does.
# So behavioral_sequences.default_pattern_enforcement defaults to "observe",
# which runs every built-in at ADVISORY; "declared" restores the levels each
# pattern asks for. Ratcheted: declared -> observe is a loosening violation.
#
#   DP-01  VACUITY GUARD: the fixture really does execute tools
#   DP-02  a previously-dead built-in (tool_rapid_fire) now fires
#   DP-03  POSITIVE CONTROL: an explicit lowercase pattern of the same shape
#          fires too — without it, DP-02 cannot show the NAMES were the cause
#   DP-04  observe mode: a SOFT built-in fires at advisory and the run COMPLETES
#   DP-05  declared mode: the same pattern BLOCKS. Without this pair, "observe"
#          is indistinguishable from the patterns still being dead
#   DP-06  an unrecognised enforcement value warns and keeps the default
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/naab-default-patterns-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT+1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_default_patterns.sh"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
STUB_PID=""
cleanup() { [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null; teardown_isolated_trust; rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"

"$NAAB" --keygen "$TEST_TMP/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/k.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/k.pem"
export FAKE_KEY_DP="fake-key-default-patterns"
source "$SCRIPT_DIR/../helpers/stub_launch.sh"

W="$TEST_TMP/w"; mkdir -p "$W"

# $1 = calls per response (5 -> tool_rapid_fire, 1 -> tool_shell_escape)
gen_fixture() {
python3 - "$W/fixture.json" "$1" <<'PYEOF'
import json, sys
n=int(sys.argv[2]); r=[]
for i in range(4):
    r.append({"tool_calls":[{"name":"peek","args":{"k":"HOME"}} for _ in range(n)]})
    r.append({"content":"ok %d" % i,"output_tokens":20,"thinking_tokens":5})
json.dump({"responses": r}, open(sys.argv[1],"w"))
PYEOF
}

# $1=tool body  $2=extra behavioral_sequences json  $3=calls per response
# echoes: "<outcome>|<fired patterns>|<tool_calls>"
run_arm() {
    gen_fixture "$3"
    start_stub "$W/fixture.json" "$W" || { echo "STUBFAIL||0"; return; }
    cat > "$W/govern.json" <<EOF
{
  "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl" },
  "behavioral_sequences": { "enabled": true${2} },
  "agents": { "worker": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT",
      "api_key_env": "FAKE_KEY_DP", "max_tokens": 200, "max_turns": 60,
      "tools_enabled": true, "tools": ["peek"],
      "system_prompt": "Report status." } }
}
EOF
    cat > "$W/t.naab" <<EOF
use agent
use env
use process
fn peek(k) { $1 }
main {
    agent.register_tool("peek", peek, {"description":"probe",
        "parameters": {"k": {"type":"string","description":"var"}}})
    let h = agent.create("worker")
    let i = 0
    while i < 4 { let r = agent.send(h, "go") i = i + 1 }
    print("DONE")
}
EOF
    (cd "$W" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
    rm -f "$W/tele.jsonl"
    local out; out=$(cd "$W" && timeout 180s "$NAAB" t.naab 2>&1)
    stop_stub
    local tc; tc=$(grep -c AGENT_TOOL_CALL "$W/tele.jsonl" 2>/dev/null || true)
    local fired; fired=$(echo "$out" | grep -oE "behavioral_sequences\.[a-z_]+" | sort -u | tr '\n' ' ')
    case "$out" in
        *"INTEGRITY BLOCK"*) echo "INFRA|$fired|$tc" ;;
        *) if echo "$out" | grep -q DONE; then echo "COMPLETED|$fired|$tc"
           else echo "BLOCKED|$fired|$tc"; fi ;;
    esac
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  Built-in BSD patterns — most of them had never fired         |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

ENVTOOL='return env.get(string(k)) ?? "u"'
PROCTOOL='let p = process.run("echo hi") return "done"'

echo -e "${CYAN}--- DP-01..03: a dead built-in now fires, and the names were why ---${NC}"
R_DEF="$(run_arm "$ENVTOOL" "" 5)"
OUT_DEF="${R_DEF%%|*}"; REST="${R_DEF#*|}"; FIRED_DEF="${REST%|*}"; TC_DEF="${REST##*|}"

if [ "$OUT_DEF" = "INFRA" ] || [ "$OUT_DEF" = "STUBFAIL" ]; then
    fail "DP-01" "harness failure ($OUT_DEF)"; VAC=0
elif [ "${TC_DEF:-0}" -gt 0 ]; then
    pass "DP-01" "vacuity guard: $TC_DEF tool calls executed"; VAC=1
else
    fail "DP-01" "no tool calls executed" "every gate below would be vacuous"; VAC=0
fi

if [ "$VAC" = "1" ]; then
    if echo "$FIRED_DEF" | grep -q "tool_rapid_fire"; then
        pass "DP-02" "built-in tool_rapid_fire fires (it never did before the name fix)"
    else
        fail "DP-02" "built-in tool_rapid_fire did not fire" "fired=[${FIRED_DEF:-none}] — default step names may have regressed to the enum spelling"
    fi
    CTL=', "patterns": [ { "name": "ctl", "sequence": ["tool_call","tool_call","tool_call","tool_call","tool_call"], "max_gap": 3, "level": "advisory" } ]'
    R_CTL="$(run_arm "$ENVTOOL" "$CTL" 5)"
    REST2="${R_CTL#*|}"; FIRED_CTL="${REST2%|*}"
    if echo "$FIRED_CTL" | grep -q "ctl"; then
        pass "DP-03" "POSITIVE CONTROL: the same shape spelled lowercase also fires"
    else
        fail "DP-03" "control pattern did not fire" "tool events may not reach the matcher at all, which would void DP-02"
    fi
else
    skip "DP-02" "vacuity guard failed"; skip "DP-03" "vacuity guard failed"
fi

echo -e "${CYAN}--- DP-04/05: observe vs declared on a SOFT built-in ---${NC}"
R_OBS="$(run_arm "$PROCTOOL" ', "default_pattern_enforcement": "observe"' 1)"
R_DEC="$(run_arm "$PROCTOOL" ', "default_pattern_enforcement": "declared"' 1)"
O_OBS="${R_OBS%%|*}"; F_OBS="$(x="${R_OBS#*|}"; echo "${x%|*}")"
O_DEC="${R_DEC%%|*}"; F_DEC="$(x="${R_DEC#*|}"; echo "${x%|*}")"

if echo "$F_OBS" | grep -q "tool_shell_escape" && [ "$O_OBS" = "COMPLETED" ]; then
    pass "DP-04" "observe: SOFT built-in fires but the run completes"
else
    fail "DP-04" "observe mode wrong" "outcome=$O_OBS fired=[${F_OBS:-none}] (want COMPLETED + tool_shell_escape)"
fi
if [ "$O_DEC" = "BLOCKED" ]; then
    pass "DP-05" "declared: the same pattern blocks"
else
    fail "DP-05" "declared mode did not block" "outcome=$O_DEC fired=[${F_DEC:-none}] — observe would then be indistinguishable from dead patterns"
fi

echo -e "${CYAN}--- DP-06: unrecognised enforcement value ---${NC}"
gen_fixture 1
if start_stub "$W/fixture.json" "$W"; then
    cat > "$W/govern.json" <<EOF
{ "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "behavioral_sequences": { "enabled": true, "default_pattern_enforcement": "bogus" },
  "agents": { "worker": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT", "api_key_env": "FAKE_KEY_DP",
      "max_tokens": 200, "max_turns": 60, "system_prompt": "Report." } } }
EOF
    cat > "$W/t.naab" <<'EOF'
use agent
main { let h = agent.create("worker") let r = agent.send(h, "go") print("DONE") }
EOF
    (cd "$W" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
    OUT6=$(cd "$W" && timeout 120s "$NAAB" t.naab 2>&1)
    stop_stub
    if echo "$OUT6" | grep -q "unknown behavioral_sequences.default_pattern_enforcement"; then
        pass "DP-06" "unrecognised value warns and names the valid ones"
    else
        fail "DP-06" "no warning for an unrecognised value" "silently keeping the default is the A1c shape"
    fi
else
    fail "DP-06" "stub failed"
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
