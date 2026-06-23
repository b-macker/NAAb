#!/usr/bin/env bash
# ============================================================
# test_transcript.sh — Agent Interaction Transcript Tests
#
# Tests the opt-in JSONL transcript that captures the full lifecycle
# of agent.create() and agent.send() calls with complete content.
#
# Group A: Config parsing & disabled-by-default
# Group B: Transcript writing (agent.create + agent.send error path)
# Group C: Agent name filtering
# Group D: Field assertions on transcript entries
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/transcript-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; TOTAL=0; FAILURES=""

pass() { local id="$1" desc="$2"; PASS_COUNT=$((PASS_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${GREEN}PASS${NC} [$id] $desc"; }
fail() { local id="$1" desc="$2" detail="${3:-}"; FAIL_COUNT=$((FAIL_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${RED}FAIL${NC} [$id] $desc"; [ -n "$detail" ] && echo -e "       ${RED}-> $detail${NC}"; FAILURES="${FAILURES}\n  [$id] $desc${detail:+ -- $detail}"; }
skip() { local id="$1" desc="$2"; SKIP_COUNT=$((SKIP_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${YELLOW}SKIP${NC} [$id] $desc"; }

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
trap 'teardown_isolated_trust; rm -rf "$TEST_TMP"' EXIT
mkdir -p "$TEST_TMP"

"$NAAB" --keygen "$TEST_TMP/test-key.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/test-key.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/test-key.pem"

setup_workdir() {
    local workdir="$TEST_TMP/work-$$-$RANDOM"
    mkdir -p "$workdir"
    echo "$workdir"
}

sign_govern() {
    local workdir="$1"
    (cd "$workdir" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
}

# Set fake API keys so agent.create() passes key validation
# (actual API calls in agent.send() will fail with HTTP errors, not key-missing errors)
export FAKE_KEY_TRANSCRIPT_TEST="fake-key-for-transcript-test"
export FAKE_KEY_A="fake-key-a"
export FAKE_KEY_B="fake-key-b"
export FAKE_KEY_DEEP="fake-key-deep"

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|         Agent Interaction Transcript Tests                    |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

# ============================================================
# Group A: Disabled by default
# ============================================================
echo -e "${CYAN}--- Group A: Disabled by default ---${NC}"

WDIR=$(setup_workdir)

cat > "$WDIR/govern.json" << 'GOVEOF'
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "telemetry": {
        "enabled": true,
        "output_file": "telemetry.jsonl"
    },
    "agents": {
        "test_agent": {
            "provider": "gemini",
            "model": "gemini-2.0-flash",
            "api_key_env": "FAKE_KEY_TRANSCRIPT_TEST",
            "max_tokens": 100,
            "max_turns": 5
        }
    }
}
GOVEOF
sign_govern "$WDIR"

cat > "$WDIR/test_disabled.naab" << 'NAABEOF'
use agent

main {
    let h = agent.create("test_agent")
    print("created")
}
NAABEOF

OUTPUT=$(cd "$WDIR" && timeout 10s "$NAAB" test_disabled.naab 2>&1) || true

# A-01: No transcript file created when transcript not configured
if [ ! -f "$WDIR/transcript.jsonl" ]; then
    pass "A-01" "No transcript.jsonl when transcript not configured"
else
    fail "A-01" "transcript.jsonl should not exist when not configured"
fi

# A-02: Script still runs fine
if echo "$OUTPUT" | grep -q "created"; then
    pass "A-02" "Agent create works without transcript config"
else
    fail "A-02" "Agent create failed without transcript config" "$(echo "$OUTPUT" | head -3)"
fi

# ============================================================
# Group B: Transcript writing (create + send error path)
# ============================================================
echo ""
echo -e "${CYAN}--- Group B: Transcript writing ---${NC}"

WDIR=$(setup_workdir)

cat > "$WDIR/govern.json" << 'GOVEOF'
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "telemetry": {
        "enabled": true,
        "output_file": "telemetry.jsonl",
        "transcript": {
            "enabled": true,
            "output_file": "transcript.jsonl"
        }
    },
    "agents": {
        "test_agent": {
            "provider": "gemini",
            "model": "gemini-2.0-flash",
            "api_key_env": "FAKE_KEY_TRANSCRIPT_TEST",
            "max_tokens": 100,
            "max_turns": 5,
            "system_prompt": "You are a test assistant for transcript verification.",
            "timeout": 5,
            "retry": { "max_attempts": 1 }
        }
    }
}
GOVEOF
sign_govern "$WDIR"

# Script that creates agent and tries to send (will fail — no API key)
cat > "$WDIR/test_transcript.naab" << 'NAABEOF'
use agent

main {
    let h = agent.create("test_agent")
    print("CREATED")
    try {
        let r = agent.send(h, "Hello transcript test")
    } catch (e) {
        print("CAUGHT:" + string(e))
    }
    print("DONE")
}
NAABEOF

OUTPUT=$(cd "$WDIR" && timeout 10s "$NAAB" test_transcript.naab 2>&1) || true

# B-01: transcript.jsonl file exists
if [ -f "$WDIR/transcript.jsonl" ]; then
    pass "B-01" "transcript.jsonl created when enabled"
else
    fail "B-01" "transcript.jsonl not created" "$(echo "$OUTPUT" | head -3)"
fi

# B-02: agent_create entry present
CREATE_COUNT=$(grep -c '"type":"agent_create"' "$WDIR/transcript.jsonl" 2>/dev/null || echo "0")
if [ "$CREATE_COUNT" -ge 1 ]; then
    pass "B-02" "agent_create entry present ($CREATE_COUNT)"
else
    fail "B-02" "agent_create entry missing" "lines: $(wc -l < "$WDIR/transcript.jsonl" 2>/dev/null)"
fi

# B-03: agent_send entry present (error path)
SEND_COUNT=$(grep -c '"type":"agent_send"' "$WDIR/transcript.jsonl" 2>/dev/null || echo "0")
if [ "$SEND_COUNT" -ge 1 ]; then
    pass "B-03" "agent_send entry present ($SEND_COUNT)"
else
    fail "B-03" "agent_send entry missing" "lines: $(wc -l < "$WDIR/transcript.jsonl" 2>/dev/null)"
fi

# B-04: agent_send entry has error:true (API key not available)
if grep '"type":"agent_send"' "$WDIR/transcript.jsonl" 2>/dev/null | grep -q '"error":true'; then
    pass "B-04" "agent_send error entry has error:true"
else
    fail "B-04" "agent_send error entry missing error:true"
fi

# B-05: agent_send entry has error_message
if grep '"type":"agent_send"' "$WDIR/transcript.jsonl" 2>/dev/null | grep -q '"error_message"'; then
    pass "B-05" "agent_send error entry has error_message"
else
    fail "B-05" "agent_send error entry missing error_message"
fi

# B-06: agent_create entry has handle_id
if grep '"type":"agent_create"' "$WDIR/transcript.jsonl" 2>/dev/null | grep -q '"handle_id"'; then
    pass "B-06" "agent_create entry has handle_id"
else
    fail "B-06" "agent_create entry missing handle_id"
fi

# B-07: agent_create entry has config snapshot
if grep '"type":"agent_create"' "$WDIR/transcript.jsonl" 2>/dev/null | grep -q '"config"'; then
    pass "B-07" "agent_create entry has config snapshot"
else
    fail "B-07" "agent_create entry missing config"
fi

# B-08: agent_create config has provider and model
if grep '"type":"agent_create"' "$WDIR/transcript.jsonl" 2>/dev/null | grep -q '"provider":"gemini"'; then
    pass "B-08" "agent_create config has correct provider"
else
    fail "B-08" "agent_create config missing/wrong provider"
fi

# B-09: agent_create has timestamp
if grep '"type":"agent_create"' "$WDIR/transcript.jsonl" 2>/dev/null | grep -q '"timestamp"'; then
    pass "B-09" "agent_create has timestamp"
else
    fail "B-09" "agent_create missing timestamp"
fi

# B-10: agent_create has run_id
if grep '"type":"agent_create"' "$WDIR/transcript.jsonl" 2>/dev/null | grep -q '"run_id"'; then
    pass "B-10" "agent_create has run_id"
else
    fail "B-10" "agent_create missing run_id"
fi

# B-11: agent_send has wall_time_ms
if grep '"type":"agent_send"' "$WDIR/transcript.jsonl" 2>/dev/null | grep -q '"wall_time_ms"'; then
    pass "B-11" "agent_send error entry has wall_time_ms"
else
    fail "B-11" "agent_send error entry missing wall_time_ms"
fi

# B-12: agent_send has agent name
if grep '"type":"agent_send"' "$WDIR/transcript.jsonl" 2>/dev/null | grep -q '"agent":"test_agent"'; then
    pass "B-12" "agent_send has correct agent name"
else
    fail "B-12" "agent_send missing/wrong agent name"
fi

# B-13: agent_send has prompt
if grep '"type":"agent_send"' "$WDIR/transcript.jsonl" 2>/dev/null | grep -q '"prompt"'; then
    pass "B-13" "agent_send has prompt field"
else
    fail "B-13" "agent_send missing prompt"
fi

# B-14: Script completed successfully (try/catch caught the error)
if echo "$OUTPUT" | grep -q "DONE"; then
    pass "B-14" "Script completed with try/catch error handling"
else
    fail "B-14" "Script did not complete" "$(echo "$OUTPUT" | tail -3)"
fi

# ============================================================
# Group C: Agent name filtering
# ============================================================
echo ""
echo -e "${CYAN}--- Group C: Agent name filtering ---${NC}"

WDIR=$(setup_workdir)

cat > "$WDIR/govern.json" << 'GOVEOF'
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "telemetry": {
        "enabled": true,
        "output_file": "telemetry.jsonl",
        "transcript": {
            "enabled": true,
            "output_file": "transcript.jsonl",
            "agents": ["agent_a"]
        }
    },
    "agents": {
        "agent_a": {
            "provider": "gemini",
            "model": "gemini-2.0-flash",
            "api_key_env": "FAKE_KEY_A",
            "max_tokens": 100,
            "max_turns": 5
        },
        "agent_b": {
            "provider": "gemini",
            "model": "gemini-2.0-flash",
            "api_key_env": "FAKE_KEY_B",
            "max_tokens": 100,
            "max_turns": 5
        }
    }
}
GOVEOF
sign_govern "$WDIR"

cat > "$WDIR/test_filter.naab" << 'NAABEOF'
use agent

main {
    let ha = agent.create("agent_a")
    let hb = agent.create("agent_b")
    print("BOTH_CREATED")
}
NAABEOF

OUTPUT=$(cd "$WDIR" && timeout 10s "$NAAB" test_filter.naab 2>&1) || true

# C-01: Only agent_a appears in transcript
A_COUNT=$(grep -c '"agent":"agent_a"' "$WDIR/transcript.jsonl" 2>/dev/null) || A_COUNT=0
B_COUNT=$(grep -c '"agent":"agent_b"' "$WDIR/transcript.jsonl" 2>/dev/null) || B_COUNT=0

if [ "$A_COUNT" -ge 1 ]; then
    pass "C-01" "Filtered agent (agent_a) appears in transcript ($A_COUNT entries)"
else
    fail "C-01" "Filtered agent (agent_a) missing from transcript"
fi

# C-02: agent_b should NOT appear (filtered out)
if [ "$B_COUNT" -eq 0 ]; then
    pass "C-02" "Non-filtered agent (agent_b) excluded from transcript"
else
    fail "C-02" "Non-filtered agent (agent_b) should not be in transcript" "found $B_COUNT entries"
fi

# C-03: Both agents still created successfully
if echo "$OUTPUT" | grep -q "BOTH_CREATED"; then
    pass "C-03" "Both agents created even with filter active"
else
    fail "C-03" "Agent creation failed with filter" "$(echo "$OUTPUT" | head -3)"
fi

# ============================================================
# Group D: Field assertions on agent_create config snapshot
# ============================================================
echo ""
echo -e "${CYAN}--- Group D: Create entry field depth ---${NC}"

# Reuse Group B transcript
WDIR2=$(setup_workdir)

cat > "$WDIR2/govern.json" << 'GOVEOF'
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "telemetry": {
        "enabled": true,
        "output_file": "telemetry.jsonl",
        "transcript": {
            "enabled": true,
            "output_file": "transcript.jsonl"
        }
    },
    "agents": {
        "deep_agent": {
            "provider": "gemini",
            "model": "gemini-2.0-flash",
            "api_key_env": "FAKE_KEY_DEEP",
            "max_tokens": 2048,
            "max_turns": 10,
            "temperature": 0.5,
            "system_prompt": "You are a deep test agent for verifying transcript config snapshots.",
            "response_format": "json",
            "risk_budget": 50,
            "standing_lease_turns": 3
        }
    }
}
GOVEOF
sign_govern "$WDIR2"

cat > "$WDIR2/test_deep.naab" << 'NAABEOF'
use agent

main {
    let h = agent.create("deep_agent")
    print("DEEP_CREATED")
}
NAABEOF

OUTPUT=$(cd "$WDIR2" && timeout 10s "$NAAB" test_deep.naab 2>&1) || true
TFILE="$WDIR2/transcript.jsonl"

# D-01: Config has model field
if grep '"type":"agent_create"' "$TFILE" 2>/dev/null | grep -q '"model":"gemini-2.0-flash"'; then
    pass "D-01" "Create config has model"
else
    fail "D-01" "Create config missing model"
fi

# D-02: Config has max_tokens
if grep '"type":"agent_create"' "$TFILE" 2>/dev/null | grep -q '"max_tokens":2048'; then
    pass "D-02" "Create config has max_tokens=2048"
else
    fail "D-02" "Create config missing/wrong max_tokens"
fi

# D-03: Config has temperature
if grep '"type":"agent_create"' "$TFILE" 2>/dev/null | grep -q '"temperature":0.5'; then
    pass "D-03" "Create config has temperature=0.5"
else
    fail "D-03" "Create config missing/wrong temperature"
fi

# D-04: Config has system_prompt (truncated)
if grep '"type":"agent_create"' "$TFILE" 2>/dev/null | grep -q '"system_prompt"'; then
    pass "D-04" "Create config has system_prompt"
else
    fail "D-04" "Create config missing system_prompt"
fi

# D-05: Config has max_turns
if grep '"type":"agent_create"' "$TFILE" 2>/dev/null | grep -q '"max_turns":10'; then
    pass "D-05" "Create config has max_turns=10"
else
    fail "D-05" "Create config missing/wrong max_turns"
fi

# D-06: Config has risk_budget
if grep '"type":"agent_create"' "$TFILE" 2>/dev/null | grep -q '"risk_budget":50'; then
    pass "D-06" "Create config has risk_budget=50"
else
    fail "D-06" "Create config missing/wrong risk_budget"
fi

# D-07: Config has standing_lease_turns
if grep '"type":"agent_create"' "$TFILE" 2>/dev/null | grep -q '"standing_lease_turns":3'; then
    pass "D-07" "Create config has standing_lease_turns=3"
else
    fail "D-07" "Create config missing/wrong standing_lease_turns"
fi

# D-08: Config has response_format
if grep '"type":"agent_create"' "$TFILE" 2>/dev/null | grep -q '"response_format":"json"'; then
    pass "D-08" "Create config has response_format=json"
else
    fail "D-08" "Create config missing/wrong response_format"
fi

# D-09: Each entry is valid single-line JSON
VALID_JSON=0
INVALID_JSON=0
while IFS= read -r line; do
    if echo "$line" | python3 -c "import sys,json; json.load(sys.stdin)" 2>/dev/null; then
        VALID_JSON=$((VALID_JSON + 1))
    else
        INVALID_JSON=$((INVALID_JSON + 1))
    fi
done < "$TFILE"
if [ "$INVALID_JSON" -eq 0 ] && [ "$VALID_JSON" -gt 0 ]; then
    pass "D-09" "All transcript entries are valid JSON ($VALID_JSON lines)"
else
    fail "D-09" "Invalid JSON in transcript" "$INVALID_JSON invalid / $VALID_JSON valid"
fi

# ============================================================
# Summary
# ============================================================
echo ""
echo -e "${CYAN}+--------------------------------------------------------------+${NC}"
echo -e "  Results: ${GREEN}$PASS_COUNT passed${NC}, ${RED}$FAIL_COUNT failed${NC}, ${YELLOW}$SKIP_COUNT skipped${NC} / $TOTAL total"
if [ -n "$FAILURES" ]; then
    echo -e "${RED}  Failures:${NC}$FAILURES"
fi
echo -e "${CYAN}+--------------------------------------------------------------+${NC}"

[ "$FAIL_COUNT" -eq 0 ]
