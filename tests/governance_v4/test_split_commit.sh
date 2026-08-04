#!/usr/bin/env bash
# ============================================================
# test_split_commit.sh — Split commit: accounting vs conversation state
#
# Uses the local agent stub (tests/helpers/agent_stub.py) via the per-agent
# api_base override so post-receive behavior is testable end-to-end.
#
# Group A: Successful sends commit history (regression)
# Group B: OA-blocked turns (DETECT) never enter handle history
# Group C: Exposure accounting counts attempted (blocked) transitions
# Group D: quarantine + inadmissible_history=exclude keeps content out of history
# Group E: New config fields parse cleanly
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_split_commit.sh"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/splitcommit-$$"

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
export FAKE_KEY_SPLIT_TEST="fake-key-split-commit-test"

sign_govern() {
    (cd "$1" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
}

start_stub() {  # $1=fixture $2=workdir
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
echo -e "${CYAN}|      Split Commit Tests (accounting vs conversation state)   |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

# ============================================================
# Group A: Successful sends commit history
# ============================================================
echo -e "${CYAN}--- Group A: Successful sends commit history ---${NC}"

WDIR="$TEST_TMP/group_a"; mkdir -p "$WDIR"
cat > "$WDIR/fixture.json" << 'EOF'
{"responses": [
  {"content": "alpha response content here", "output_tokens": 20},
  {"content": "beta response content here", "output_tokens": 20}
]}
EOF
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "A-00" "stub failed to start"; exit 1; }

cat > "$WDIR/govern.json" << GOVEOF
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "agents": {
        "test_agent": {
            "provider": "gemini",
            "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": "FAKE_KEY_SPLIT_TEST",
            "max_tokens": 100,
            "max_turns": 20
        }
    }
}
GOVEOF
sign_govern "$WDIR"

cat > "$WDIR/test.naab" << 'NAABEOF'
use agent

main {
    let h = agent.create("test_agent")
    let r1 = agent.send(h, "first message")
    let r2 = agent.send(h, "second message")
    let msgs = agent.messages(h)
    print("CONTENT1=" + string(r1.get("content")))
    print("MSGS=" + string(msgs.length()))
}
NAABEOF

OUTPUT=$(cd "$WDIR" && timeout 30s "$NAAB" test.naab 2>&1) || true
stop_stub

if echo "$OUTPUT" | grep -q "CONTENT1=alpha response content here"; then
    pass "A-01" "Stub-backed send returns fixture content"
else
    fail "A-01" "Send did not return stub content" "$(echo "$OUTPUT" | head -3)"
fi
if echo "$OUTPUT" | grep -q "MSGS=4"; then
    pass "A-02" "Two successful sends commit 4 history messages"
else
    fail "A-02" "History length wrong after 2 sends" "$(echo "$OUTPUT" | grep MSGS)"
fi

# ============================================================
# Group B: OA-blocked (DETECT) turns never enter history
# Coherence is driven down by json contract failures feeding CDD,
# then output_admissibility (threshold 0.95, action=block, level=detect)
# blocks turns. Blocked turns must NOT append to handle history.
# ============================================================
echo -e "${CYAN}--- Group B: Blocked turns excluded from history ---${NC}"

WDIR="$TEST_TMP/group_b"; mkdir -p "$WDIR"
cat > "$WDIR/fixture.json" << 'EOF'
{"responses": [
  {"content": "this is definitely not valid json output at all", "output_tokens": 30}
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
            "action": "block",
            "level": "detect"
        }
    },
    "agents": {
        "test_agent": {
            "provider": "gemini",
            "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": "FAKE_KEY_SPLIT_TEST",
            "max_tokens": 100,
            "max_turns": 20,
            "response_format": "json"
        }
    }
}
GOVEOF
sign_govern "$WDIR"

cat > "$WDIR/test.naab" << 'NAABEOF'
use agent

main {
    let h = agent.create("test_agent")
    let ok = 0
    let blocked = 0
    for i in 0..8 {
        try {
            let r = agent.send(h, "please respond turn " + string(i))
            ok = ok + 1
        } catch (e) {
            blocked = blocked + 1
        }
    }
    let msgs = agent.messages(h)
    print("OK=" + string(ok))
    print("BLOCKED=" + string(blocked))
    print("MSGS=" + string(msgs.length()))
    if msgs.length() == ok * 2 {
        print("SPLIT_COMMIT_PASS")
    } else {
        print("SPLIT_COMMIT_FAIL")
    }
}
NAABEOF

OUTPUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1) || true
stop_stub

if echo "$OUTPUT" | grep -q "BLOCKED=[1-9]"; then
    pass "B-01" "OA gate (DETECT) blocked at least one low-coherence turn"
else
    fail "B-01" "No turn was blocked — OA gate never fired" "$(echo "$OUTPUT" | grep -E 'OK=|BLOCKED=' | head -2)"
fi
if echo "$OUTPUT" | grep -q "SPLIT_COMMIT_PASS"; then
    pass "B-02" "History length == 2 x successful sends (blocked turns excluded)"
else
    fail "B-02" "Blocked turns leaked into handle history" "$(echo "$OUTPUT" | grep -E 'OK=|BLOCKED=|MSGS=' | head -3)"
fi
# action=block emits OUTPUT_ADMISSIBILITY_EVAL result=fail inside the gate
# before the throw (OUTPUT_INADMISSIBLE is quarantine/attest only)
if grep -q "OUTPUT_ADMISSIBILITY_EVAL" "$WDIR/telemetry.jsonl" 2>/dev/null && \
   grep "OUTPUT_ADMISSIBILITY_EVAL" "$WDIR/telemetry.jsonl" | grep -q '"result": *"fail"'; then
    pass "B-03" "OUTPUT_ADMISSIBILITY_EVAL fail telemetry emitted for blocked turns"
else
    fail "B-03" "No failing OUTPUT_ADMISSIBILITY_EVAL telemetry found"
fi

# ============================================================
# Group C: Exposure accounting counts blocked attempts
# ============================================================
echo -e "${CYAN}--- Group C: Exposure accounting counts attempts ---${NC}"

MAX_ACTIONS=$(python3 - "$WDIR/telemetry.jsonl" << 'PYEOF'
import json, sys
best = -1
try:
    with open(sys.argv[1]) as f:
        for line in f:
            try:
                ev = json.loads(line)
            except json.JSONDecodeError:
                continue
            if ev.get("event_type") == "ADMISSION_EVAL":
                try:
                    best = max(best, int(ev.get("autonomous_actions", -1)))
                except (TypeError, ValueError):
                    pass
except OSError:
    pass
print(best)
PYEOF
)
# 8 attempts; ADMISSION_EVAL fires before that turn's recordAutonomousAction,
# so the last event must show at least 7 prior attempts (including blocked ones).
if [ "$MAX_ACTIONS" -ge 7 ] 2>/dev/null; then
    pass "C-01" "ADMISSION_EVAL shows blocked attempts in autonomous_actions ($MAX_ACTIONS)"
else
    fail "C-01" "Exposure count missing blocked attempts (max autonomous_actions=$MAX_ACTIONS, want >=7)"
fi

# ============================================================
# Group D: quarantine + inadmissible_history=exclude
# ============================================================
echo -e "${CYAN}--- Group D: quarantine + inadmissible_history=exclude ---${NC}"

WDIR="$TEST_TMP/group_d"; mkdir -p "$WDIR"
cat > "$WDIR/fixture.json" << 'EOF'
{"responses": [
  {"content": "still not json in any way shape or form", "output_tokens": 30}
]}
EOF
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "D-00" "stub failed to start"; exit 1; }

cat > "$WDIR/govern.json" << GOVEOF
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "behavioral_sequences": { "enabled": true },
    "context_drift": { "enabled": true, "check_interval_turns": 1 },
    "circuit_breaker": {
        "enabled": true,
        "output_admissibility": {
            "enabled": true,
            "threshold": 0.95,
            "action": "quarantine",
            "inadmissible_history": "exclude",
            "max_quarantine_streak": 0
        }
    },
    "agents": {
        "test_agent": {
            "provider": "gemini",
            "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": "FAKE_KEY_SPLIT_TEST",
            "max_tokens": 100,
            "max_turns": 20,
            "response_format": "json"
        }
    }
}
GOVEOF
sign_govern "$WDIR"

cat > "$WDIR/test.naab" << 'NAABEOF'
use agent

main {
    let h = agent.create("test_agent")
    let admissible = 0
    let quarantined = 0
    let got_content = 0
    for i in 0..8 {
        let r = agent.send(h, "respond please turn " + string(i))
        if string(r.get("content")).length() > 0 { got_content = got_content + 1 }
        let adm = r.get("admissibility")
        if adm != null {
            if adm.get("admissible") {
                admissible = admissible + 1
            } else {
                quarantined = quarantined + 1
            }
        } else {
            admissible = admissible + 1
        }
    }
    let msgs = agent.messages(h)
    print("ADMISSIBLE=" + string(admissible))
    print("QUARANTINED=" + string(quarantined))
    print("GOT_CONTENT=" + string(got_content))
    print("MSGS=" + string(msgs.length()))
    if msgs.length() == admissible * 2 {
        print("EXCLUDE_PASS")
    } else {
        print("EXCLUDE_FAIL")
    }
}
NAABEOF

OUTPUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1) || true
stop_stub

if echo "$OUTPUT" | grep -q "QUARANTINED=[1-9]"; then
    pass "D-01" "Quarantine fired for low-coherence turns"
else
    fail "D-01" "No turn was quarantined" "$(echo "$OUTPUT" | grep -E 'ADMISSIBLE=|QUARANTINED=' | head -2)"
fi
if echo "$OUTPUT" | grep -q "GOT_CONTENT=8"; then
    pass "D-02" "Quarantined responses still returned to caller"
else
    fail "D-02" "Quarantined responses were not returned" "$(echo "$OUTPUT" | grep GOT_CONTENT)"
fi
if echo "$OUTPUT" | grep -q "EXCLUDE_PASS"; then
    pass "D-03" "inadmissible_history=exclude kept quarantined content out of history"
else
    fail "D-03" "Quarantined content leaked into history" "$(echo "$OUTPUT" | grep -E 'MSGS=|ADMISSIBLE=' | head -2)"
fi

# ============================================================
# Group E: Config parse of new fields
# ============================================================
echo -e "${CYAN}--- Group E: Config parse ---${NC}"

WDIR="$TEST_TMP/group_e"; mkdir -p "$WDIR"
cat > "$WDIR/govern.json" << 'GOVEOF'
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "circuit_breaker": {
        "enabled": true,
        "output_admissibility": {
            "enabled": true,
            "threshold": 0.8,
            "action": "quarantine",
            "inadmissible_history": "exclude",
            "gate_tool_calls": true
        }
    },
    "agents": {
        "test_agent": {
            "provider": "gemini",
            "model": "stub-model",
            "api_base": "http://127.0.0.1:19999",
            "api_key_env": "FAKE_KEY_SPLIT_TEST",
            "propose_candidates_max": 3
        }
    }
}
GOVEOF
sign_govern "$WDIR"

cat > "$WDIR/test.naab" << 'NAABEOF'
main {
    print("config parsed fine")
}
NAABEOF

OUTPUT=$(cd "$WDIR" && timeout 15s "$NAAB" test.naab 2>&1)
RC=$?
if [ $RC -eq 0 ] && echo "$OUTPUT" | grep -q "config parsed fine"; then
    pass "E-01" "New config fields (inadmissible_history, gate_tool_calls, propose_candidates_max, api_base) parse"
else
    fail "E-01" "Config with new fields failed to load (rc=$RC)" "$(echo "$OUTPUT" | head -3)"
fi

# Invalid api_base (non-loopback http) is ignored with a warning, not fatal
cat > "$WDIR/govern2.json" << 'GOVEOF'
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "agents": {
        "test_agent": {
            "provider": "gemini",
            "model": "stub-model",
            "api_base": "http://evil.example.com",
            "api_key_env": "FAKE_KEY_SPLIT_TEST"
        }
    }
}
GOVEOF
mv "$WDIR/govern.json" "$WDIR/govern.json.bak"
cp "$WDIR/govern2.json" "$WDIR/govern.json"
sign_govern "$WDIR"
OUTPUT=$(cd "$WDIR" && timeout 15s "$NAAB" test.naab 2>&1)
if echo "$OUTPUT" | grep -q "api_base ignored"; then
    pass "E-02" "Non-loopback http api_base rejected with warning"
else
    fail "E-02" "Insecure api_base was not rejected" "$(echo "$OUTPUT" | head -3)"
fi

# ============================================================
echo ""
echo -e "${CYAN}==============================================${NC}"
echo -e "  Total: $TOTAL  ${GREEN}Pass: $PASS_COUNT${NC}  ${RED}Fail: $FAIL_COUNT${NC}  ${YELLOW}Skip: $SKIP_COUNT${NC}"
[ -n "$FAILURES" ] && echo -e "${RED}Failures:$FAILURES${NC}"
echo -e "${CYAN}==============================================${NC}"
[ $FAIL_COUNT -eq 0 ]
